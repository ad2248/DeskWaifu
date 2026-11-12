use deskwaifu_cubism_core::{latest_moc_version, version, Error, Moc};

#[test]
fn core_version_is_available() {
    assert_ne!(version(), 0);
    assert_ne!(latest_moc_version(), 0);
}

#[test]
fn invalid_inputs_are_rejected() {
    for bytes in [&[][..], &b"MOC3"[..], &b"not a model"[..], &[0u8; 256][..]] {
        assert!(matches!(Moc::from_bytes(bytes), Err(Error::InvalidMoc)));
    }
    // Correct magic does not make a moc valid: exercise the native consistency checker.
    let mut invalid = vec![0; 256];
    invalid[..4].copy_from_slice(b"MOC3");
    invalid[4] = 1;
    assert!(matches!(Moc::from_bytes(&invalid), Err(Error::InvalidMoc)));
}

#[test]
fn external_moc_load_initialize_update_and_parameters() {
    let Some(path) = std::env::var_os("CUBISM_TEST_MOC") else {
        eprintln!("Skipping external fixture test: set CUBISM_TEST_MOC to a licensed .moc3 file");
        return;
    };
    let path = std::path::Path::new(&path);
    assert!(
        path.is_file(),
        "CUBISM_TEST_MOC does not exist: {}",
        path.display()
    );
    let bytes = std::fs::read(path).expect("read CUBISM_TEST_MOC");
    let moc = Moc::from_bytes(&bytes).expect("validate and revive external moc");
    let mut model = moc.create_model().expect("initialize external model");
    let mut second_model = moc.create_model().expect("initialize independent model");
    println!(
        "Core version: {:#010x}; moc version: {}; fixture bytes: {}; canvas: {:?}; parameters: {}",
        version(),
        moc.format_version(),
        bytes.len(),
        model.canvas(),
        model.parameters().len()
    );
    drop(moc); // Models must retain source storage, even after the public Moc is gone.
    drop(bytes);
    let canvas = model.canvas();
    assert!(canvas
        .size_in_pixels
        .iter()
        .all(|value| value.is_finite() && *value > 0.0));
    assert!(canvas.pixels_per_unit.is_finite() && canvas.pixels_per_unit > 0.0);
    assert!(!model.parameters().is_empty());
    let owned_metadata = model.parameters().to_vec();
    for (index, parameter) in owned_metadata.iter().enumerate() {
        assert!(!parameter.id.is_empty());
        assert_eq!(model.parameter_index(&parameter.id), Some(index));
        assert_eq!(model.parameter_value(index).unwrap(), parameter.default);
        model.set_parameter_value(index, parameter.minimum).unwrap();
        assert_eq!(model.parameter_value(index).unwrap(), parameter.minimum);
        assert_eq!(
            second_model.parameter_value(index).unwrap(),
            parameter.default
        );
        model.set_parameter_value(index, parameter.maximum).unwrap();
        assert_eq!(model.parameter_value(index).unwrap(), parameter.maximum);
        for invalid in [
            f32::NAN,
            f32::INFINITY,
            f32::NEG_INFINITY,
            parameter.minimum - 1.0,
            parameter.maximum + 1.0,
        ] {
            assert_eq!(
                model.set_parameter_value(index, invalid),
                Err(Error::ParameterValueOutOfBounds)
            );
        }
        model.set_parameter_value(index, parameter.default).unwrap();
    }
    assert_eq!(
        model.parameter_value(usize::MAX),
        Err(Error::ParameterIndexOutOfBounds)
    );
    assert_eq!(
        model.set_parameter_value(model.parameters().len(), 0.0),
        Err(Error::ParameterIndexOutOfBounds)
    );
    assert_eq!(model.parameter_index("__missing_parameter__"), None);
    for _ in 0..10 {
        model.update();
        second_model.update();
    }
    assert_eq!(model.parameters(), owned_metadata);
    drop(model);
    drop(second_model);
    assert!(!owned_metadata[0].id.is_empty()); // Metadata does not borrow native storage.

    // Truncation must fail validation, never reach model initialization.
    let original = std::fs::read(path).unwrap();
    for length in [0, 4, 63, 64, original.len() / 2, original.len() - 1] {
        assert!(
            matches!(Moc::from_bytes(&original[..length]), Err(Error::InvalidMoc)),
            "accepted truncated moc of {length} bytes"
        );
    }
}
