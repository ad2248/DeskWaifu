//! Filesystem-backed discovery of model3 descriptors.

use std::collections::HashSet;
use std::fs;
use std::path::Path;

const MODEL_SUFFIX: &str = ".model3.json";

/// A model descriptor found directly inside a model directory.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ModelDescriptor {
    /// Canonical model-directory path plus the JSON filename (file symlinks are not resolved).
    pub id: String,
    /// Name of the direct child directory as encountered under the root.
    pub name: String,
    /// Canonical model-directory path, with a trailing slash.
    pub directory: String,
    pub json_name: String,
}

/// Scans direct child directories of each root, then sorts by name and ID and deduplicates by ID.
/// Inaccessible roots and entries are skipped, matching the desktop catalog behavior.
pub fn scan_model_catalog(roots: &[impl AsRef<Path>]) -> Vec<ModelDescriptor> {
    let mut result = Vec::new();
    for root in roots {
        let Ok(base) = fs::canonicalize(root.as_ref()) else {
            continue;
        };
        let Ok(entries) = fs::read_dir(&base) else {
            continue;
        };
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().into_owned();
            if name == "." || name == ".." {
                continue;
            }
            let Ok(model_dir) = fs::canonicalize(entry.path()) else {
                continue;
            };
            if !model_dir.is_dir() {
                continue;
            }
            let Ok(files) = fs::read_dir(&model_dir) else {
                continue;
            };
            let directory = with_trailing_separator(&model_dir);
            for file in files.flatten() {
                let json_name = file.file_name().to_string_lossy().into_owned();
                if json_name.len() <= MODEL_SUFFIX.len() || !json_name.ends_with(MODEL_SUFFIX) {
                    continue;
                }
                if fs::metadata(file.path()).is_ok_and(|metadata| metadata.is_file()) {
                    result.push(ModelDescriptor {
                        id: format!("{directory}{json_name}"),
                        name: name.clone(),
                        directory: directory.clone(),
                        json_name,
                    });
                }
            }
        }
    }
    result.sort_by(|a, b| a.name.cmp(&b.name).then_with(|| a.id.cmp(&b.id)));
    let mut seen = HashSet::new();
    result.retain(|model| seen.insert(model.id.clone()));
    result
}

fn with_trailing_separator(path: &Path) -> String {
    let mut value = path.to_string_lossy().into_owned();
    if !value.ends_with(std::path::MAIN_SEPARATOR) {
        value.push(std::path::MAIN_SEPARATOR);
    }
    value
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::symlink;

    fn descriptor(id: &Path, name: &str, json_name: &str) -> ModelDescriptor {
        let directory = with_trailing_separator(id.parent().unwrap());
        ModelDescriptor {
            id: format!("{directory}{json_name}"),
            name: name.to_owned(),
            directory,
            json_name: json_name.to_owned(),
        }
    }

    #[test]
    fn sorts_and_deduplicates_aliases_across_roots_and_repeated_roots() {
        let temp = tempfile::tempdir().unwrap();
        let first = temp.path().join("first");
        let second = temp.path().join("second");
        fs::create_dir_all(first.join("Twin")).unwrap();
        fs::create_dir_all(second.join("Twin")).unwrap();
        fs::write(first.join("Twin/alpha.model3.json"), "{}").unwrap();
        fs::write(second.join("Twin/beta.model3.json"), "{}").unwrap();
        symlink(
            first.join("Twin/alpha.model3.json"),
            second.join("Twin/alpha.model3.json"),
        )
        .unwrap();
        symlink(first.join("Twin"), first.join("Aalias")).unwrap();
        symlink(first.join("Twin"), second.join("Zalias")).unwrap();

        let found = scan_model_catalog(&[&second, &temp.path().join("missing"), &first, &second]);
        let expected = vec![
            descriptor(
                &first.join("Twin/alpha.model3.json"),
                "Aalias",
                "alpha.model3.json",
            ),
            descriptor(
                &second.join("Twin/alpha.model3.json"),
                "Twin",
                "alpha.model3.json",
            ),
            descriptor(
                &second.join("Twin/beta.model3.json"),
                "Twin",
                "beta.model3.json",
            ),
        ];
        assert_eq!(found, expected);
    }

    #[test]
    fn skips_invalid_or_missing_files_and_non_directories() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path().join("root");
        let model = root.join("Model");
        fs::create_dir_all(&model).unwrap();
        fs::write(model.join("valid.model3.json"), "{}").unwrap();
        fs::write(model.join(".model3.json"), "{}").unwrap();
        fs::write(model.join("notes.txt"), "skip").unwrap();
        fs::create_dir(model.join("nested.model3.json")).unwrap();
        fs::write(root.join("not-a-directory.model3.json"), "skip").unwrap();
        symlink(
            temp.path().join("missing.json"),
            model.join("broken.model3.json"),
        )
        .unwrap();

        assert_eq!(
            scan_model_catalog(&[&root, &temp.path().join("missing-root")]),
            [descriptor(
                &model.join("valid.model3.json"),
                "Model",
                "valid.model3.json"
            )]
        );
    }
}
