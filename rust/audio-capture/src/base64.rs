//! Unpadded base64url, matching `QByteArray::toBase64(Base64UrlEncoding)`.
//!
//! The C++ capture hands the GUI such a token as `id`, and the GUI stores it
//! verbatim in `device_id`. Qt's `Base64UrlEncoding` alone omits `=` padding,
//! so this encoder does too.

const ALPHABET: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/// Encodes `input` as unpadded base64url.
pub fn encode(input: &[u8]) -> String {
    let mut out = String::with_capacity(input.len().div_ceil(3) * 4);
    for chunk in input.chunks(3) {
        let bytes = [
            chunk[0],
            chunk.get(1).copied().unwrap_or(0),
            chunk.get(2).copied().unwrap_or(0),
        ];
        let value = (u32::from(bytes[0]) << 16) | (u32::from(bytes[1]) << 8) | u32::from(bytes[2]);
        out.push(ALPHABET[(value >> 18) as usize & 63] as char);
        out.push(ALPHABET[(value >> 12) as usize & 63] as char);
        if chunk.len() > 1 {
            out.push(ALPHABET[(value >> 6) as usize & 63] as char);
        }
        if chunk.len() > 2 {
            out.push(ALPHABET[value as usize & 63] as char);
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn matches_qt_base64url_encoding() {
        // Reference values from `base64.urlsafe_b64encode(...).rstrip("=")`.
        assert_eq!(encode(b""), "");
        assert_eq!(encode(b"f"), "Zg");
        assert_eq!(encode(b"fo"), "Zm8");
        assert_eq!(encode(b"foo"), "Zm9v");
        assert_eq!(encode(b"offline-device"), "b2ZmbGluZS1kZXZpY2U");
        assert_eq!(encode(b"alsa:hw:0,0"), "YWxzYTpodzowLDA");
        // The url-safe alphabet, not the standard one.
        assert_eq!(encode(&[0xfb, 0xff]), "-_8");
    }
}
