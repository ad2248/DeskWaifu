//! Greedy CTC postprocessing matching `OcrEngine.cpp::ReadLine`.
//!
//! This decodes supplied recognition probabilities; it does not run inference.

/// Text and mean probability of emitted tokens (zero when none are emitted).
#[derive(Clone, Debug, PartialEq)]
pub struct DecodedLine {
    pub text: String,
    pub confidence: f64,
}

/// Invalid recognition tensor or dictionary input.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DecodeError {
    InvalidDimensions,
    DictionaryMismatch,
    LengthOverflow,
    LengthMismatch,
    NonFiniteProbability,
}

/// Decode a row-major `[timesteps, classes]` probability array.
///
/// Both dimensions must be positive and the dictionary must contain one string
/// per class, with class zero reserved for blank. Entries may contain multiple
/// Unicode characters. Ties select the first class. Consecutive equal classes
/// collapse, but a blank separates repetitions. Confidence averages only the
/// selected probabilities that actually emit a dictionary entry, not every step
/// or every Unicode character. Finite values are accepted without normalization.
pub fn decode_ctc<S: AsRef<str>>(
    probabilities: &[f32],
    timesteps: usize,
    classes: usize,
    characters: &[S],
) -> Result<DecodedLine, DecodeError> {
    if timesteps == 0 || classes == 0 {
        return Err(DecodeError::InvalidDimensions);
    }
    if classes != characters.len() {
        return Err(DecodeError::DictionaryMismatch);
    }
    let expected = timesteps
        .checked_mul(classes)
        .ok_or(DecodeError::LengthOverflow)?;
    if probabilities.len() != expected {
        return Err(DecodeError::LengthMismatch);
    }
    if probabilities.iter().any(|value| !value.is_finite()) {
        return Err(DecodeError::NonFiniteProbability);
    }

    let mut text = String::new();
    let mut score = 0.0_f64;
    let mut count = 0_usize;
    let mut previous = None;
    for row in probabilities.chunks_exact(classes) {
        let mut index = 0;
        for candidate in 1..classes {
            if row[candidate] > row[index] {
                index = candidate;
            }
        }
        if index != 0 && previous != Some(index) {
            text.push_str(characters[index].as_ref());
            score += f64::from(row[index]);
            count += 1;
        }
        previous = Some(index);
    }
    Ok(DecodedLine {
        text,
        confidence: if count == 0 {
            0.0
        } else {
            score / count as f64
        },
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn repeats_collapse_but_blank_separates_and_only_emissions_are_scored() {
        let data = [
            0.1, 0.8, 0.1, // a emitted
            0.0, 0.9, 0.1, // repeated a ignored
            1.0, 0.0, 0.0, // blank
            0.1, 0.6, 0.3, // a emitted again
            0.0, 0.3, 0.7, // b emitted
        ];
        let result = decode_ctc(&data, 5, 3, &["", "a", "b"]).unwrap();
        assert_eq!(result.text, "aab");
        assert_eq!(
            result.confidence,
            (f64::from(0.8_f32) + f64::from(0.6_f32) + f64::from(0.7_f32)) / 3.0
        );
    }

    #[test]
    fn ties_choose_first_class_including_blank() {
        let result = decode_ctc(&[0.5, 0.5, 0.0, 0.0, 0.5, 0.5], 2, 3, &["", "a", "b"]).unwrap();
        assert_eq!(result.text, "a");
        assert_eq!(result.confidence, 0.5);
    }

    #[test]
    fn unicode_entries_are_tokens_not_individual_characters() {
        let result = decode_ctc(
            &[0.0, 0.75, 0.25, 0.0, 0.0, 1.0],
            2,
            3,
            &["blank", "你好", "🦀é"],
        )
        .unwrap();
        assert_eq!(result.text, "你好🦀é");
        assert_eq!(result.confidence, 0.875);
    }

    #[test]
    fn all_blank_has_empty_text_and_zero_confidence() {
        assert_eq!(
            decode_ctc(&[1.0, 1.0], 2, 1, &["ignored"]),
            Ok(DecodedLine {
                text: String::new(),
                confidence: 0.0,
            })
        );
    }

    #[test]
    fn invalid_shapes_and_checked_length_are_rejected() {
        for (steps, classes) in [(0, 1), (1, 0), (0, 0)] {
            assert_eq!(
                decode_ctc(&[], steps, classes, &[""]),
                Err(DecodeError::InvalidDimensions)
            );
        }
        assert_eq!(
            decode_ctc(&[], 1, 2, &[""]),
            Err(DecodeError::DictionaryMismatch)
        );
        assert_eq!(
            decode_ctc(&[], usize::MAX, 2, &["", "a"]),
            Err(DecodeError::LengthOverflow)
        );
        for data in [&[][..], &[1.0, 0.0, 1.0][..]] {
            assert_eq!(
                decode_ctc(data, 1, 2, &["", "a"]),
                Err(DecodeError::LengthMismatch)
            );
        }
    }

    #[test]
    fn nonfinite_values_even_in_unselected_classes_are_rejected() {
        for invalid in [f32::NAN, f32::INFINITY, f32::NEG_INFINITY] {
            assert_eq!(
                decode_ctc(&[1.0, invalid], 1, 2, &["", "a"]),
                Err(DecodeError::NonFiniteProbability)
            );
        }
    }
}
