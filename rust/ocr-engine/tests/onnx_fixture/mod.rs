//! Minimal ONNX encoder for test fixtures.
//!
//! It writes a `ModelProto` whose graph is a single `Constant` node, exactly the
//! shape used by `plugins/perception/tests/test_ocr_native_models.py`. Nothing
//! here is used at runtime; it exists so tests can exercise the real ONNX
//! Runtime path without Python, downloads or real weights.

fn varint(mut value: u64) -> Vec<u8> {
    let mut out = Vec::new();
    while value > 127 {
        out.push((value as u8 & 127) | 128);
        value >>= 7;
    }
    out.push(value as u8);
    out
}

/// A `varint`-encoded field: `field << 3 | wire`.
fn integer(field: u32, value: u64) -> Vec<u8> {
    let mut out = varint(u64::from(field << 3));
    out.extend(varint(value));
    out
}

/// A length-delimited field.
fn bytes(field: u32, value: &[u8]) -> Vec<u8> {
    let mut out = varint(u64::from(field << 3) | 2);
    out.extend(varint(value.len() as u64));
    out.extend_from_slice(value);
    out
}

/// `ValueInfoProto` with a concrete or symbolic (`-1`) shape.
fn value_info(name: &str, shape: &[i64], element_type: u64) -> Vec<u8> {
    let mut dimensions = Vec::new();
    for (index, size) in shape.iter().enumerate() {
        let dimension = if *size > 0 {
            integer(1, *size as u64)
        } else {
            bytes(2, format!("dynamic_{index}").as_bytes())
        };
        dimensions.extend(bytes(1, &dimension));
    }
    let mut tensor_type = integer(1, element_type);
    tensor_type.extend(bytes(2, &dimensions));
    let mut out = bytes(1, name.as_bytes());
    out.extend(bytes(2, &bytes(1, &tensor_type)));
    out
}

/// Encode a constant-graph model (ONNX IR 8, opset 13).
///
/// `declared_shape` lets a test lie about the output shape so the engine's
/// `ValidateSession` checks can be exercised.
pub fn constant_model(
    input_shape: &[i64],
    output_shape: &[i64],
    values: &[f32],
    declared_shape: Option<&[i64]>,
    element_type: u64,
) -> Vec<u8> {
    let mut tensor = Vec::new();
    for size in output_shape {
        tensor.extend(integer(1, *size as u64));
    }
    tensor.extend(integer(2, 1)); // data_type = FLOAT
    let mut raw = Vec::with_capacity(values.len() * 4);
    for value in values {
        raw.extend(value.to_le_bytes());
    }
    tensor.extend(bytes(9, &raw)); // raw_data

    let mut attribute = bytes(1, b"value");
    attribute.extend(bytes(5, &tensor)); // type = TENSOR
    attribute.extend(integer(20, 4)); // AttributeProto::Type::TENSOR

    let mut node = bytes(2, b"output");
    node.extend(bytes(4, b"Constant"));
    node.extend(bytes(5, &attribute));

    let mut graph = bytes(1, &node);
    graph.extend(bytes(2, b"ocr-test"));
    graph.extend(bytes(11, &value_info("input", input_shape, element_type)));
    graph.extend(bytes(
        12,
        &value_info(
            "output",
            declared_shape.unwrap_or(output_shape),
            element_type,
        ),
    ));

    let mut model = integer(1, 8); // IR version
    model.extend(bytes(7, &graph));
    model.extend(bytes(8, &integer(2, 13))); // opset 13
    model
}
