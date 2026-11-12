//! Integration tests for the pure `.model3.json` metadata parser.

use deskwaifu_model_domain::model3::{
    Model3, Model3Error, Motion, MotionGroup, MotionRef, EYE_BLINK_GROUP, LIP_SYNC_GROUP,
};

/// The real Cubism sample `Haru.model3.json`, copied verbatim so the test runs
/// without an authorized SDK checkout. `FileReferences.UserData` is part of
/// the original file and doubles as an unknown-key check.
const HARU: &str = r#"{
	"Version": 3,
	"FileReferences": {
		"Moc": "Haru.moc3",
		"Textures": [
			"Haru.2048/texture_00.png",
			"Haru.2048/texture_01.png"
		],
		"Physics": "Haru.physics3.json",
		"Pose": "Haru.pose3.json",
		"DisplayInfo": "Haru.cdi3.json",
		"Expressions": [
			{
				"Name": "F01",
				"File": "expressions/F01.exp3.json"
			},
			{
				"Name": "F02",
				"File": "expressions/F02.exp3.json"
			},
			{
				"Name": "F03",
				"File": "expressions/F03.exp3.json"
			},
			{
				"Name": "F04",
				"File": "expressions/F04.exp3.json"
			},
			{
				"Name": "F05",
				"File": "expressions/F05.exp3.json"
			},
			{
				"Name": "F06",
				"File": "expressions/F06.exp3.json"
			},
			{
				"Name": "F07",
				"File": "expressions/F07.exp3.json"
			},
			{
				"Name": "F08",
				"File": "expressions/F08.exp3.json"
			}
		],
		"Motions": {
			"Idle": [
				{
					"File": "motions/haru_g_idle.motion3.json",
					"FadeInTime": 0.5,
					"FadeOutTime": 0.5
				},
				{
					"File": "motions/haru_g_m15.motion3.json",
					"FadeInTime": 0.5,
					"FadeOutTime": 0.5
				}
			],
			"TapBody": [
				{
					"File": "motions/haru_g_m26.motion3.json",
					"FadeInTime": 0.5,
					"FadeOutTime": 0.5,
					"Sound": "sounds/haru_talk_13.wav"
				},
				{
					"File": "motions/haru_g_m06.motion3.json",
					"FadeInTime": 0.5,
					"FadeOutTime": 0.5,
					"Sound": "sounds/haru_Info_14.wav"
				},
				{
					"File": "motions/haru_g_m20.motion3.json",
					"FadeInTime": 0.5,
					"FadeOutTime": 0.5,
					"Sound": "sounds/haru_normal_6.wav"
				},
				{
					"File": "motions/haru_g_m09.motion3.json",
					"FadeInTime": 0.5,
					"FadeOutTime": 0.5,
					"Sound": "sounds/haru_Info_04.wav"
				}
			]
		},
		"UserData": "Haru.userdata3.json"
	},
	"Groups": [
		{
			"Target": "Parameter",
			"Name": "EyeBlink",
			"Ids": [
				"ParamEyeLOpen",
				"ParamEyeROpen"
			]
		},
		{
			"Target": "Parameter",
			"Name": "LipSync",
			"Ids": [
				"ParamMouthOpenY"
			]
		}
	],
	"HitAreas": [
		{
			"Id": "HitArea",
			"Name": "Head"
		},
		{
			"Id": "HitArea2",
			"Name": "Body"
		}
	]
}"#;

#[test]
fn parses_real_haru_descriptor() {
    let model = Model3::parse(HARU).unwrap();
    assert_eq!(model.version, Some(3));
    assert_eq!(model.model_file_name(), "Haru.moc3");
    assert_eq!(
        model.texture_file_names(),
        [
            "Haru.2048/texture_00.png".to_owned(),
            "Haru.2048/texture_01.png".to_owned(),
        ]
    );
    assert_eq!(
        model.file_references.physics.as_deref(),
        Some("Haru.physics3.json")
    );
    assert_eq!(
        model.file_references.pose.as_deref(),
        Some("Haru.pose3.json")
    );
    assert_eq!(
        model.file_references.display_info.as_deref(),
        Some("Haru.cdi3.json")
    );

    assert_eq!(
        model.expression_names(),
        ["F01", "F02", "F03", "F04", "F05", "F06", "F07", "F08"]
    );
    assert_eq!(
        model.file_references.expressions[0].file,
        "expressions/F01.exp3.json"
    );

    assert_eq!(
        model
            .file_references
            .motions
            .iter()
            .map(|group| group.name.as_str())
            .collect::<Vec<_>>(),
        ["Idle", "TapBody"]
    );
    let idle = &model.file_references.motions[0];
    assert_eq!(
        idle.motions,
        [
            Motion {
                file: "motions/haru_g_idle.motion3.json".into(),
                fade_in_time: Some(0.5),
                fade_out_time: Some(0.5),
                sound: None,
            },
            Motion {
                file: "motions/haru_g_m15.motion3.json".into(),
                fade_in_time: Some(0.5),
                fade_out_time: Some(0.5),
                sound: None,
            },
        ]
    );
    assert_eq!(
        model.file_references.motions[1].motions[0].sound.as_deref(),
        Some("sounds/haru_talk_13.wav")
    );

    // The flattened list is exactly what LAppModel::GetMotionList() produces.
    assert_eq!(
        model.motion_list(),
        [
            MotionRef {
                group: "Idle",
                index: 0,
                file: "motions/haru_g_idle.motion3.json",
            },
            MotionRef {
                group: "Idle",
                index: 1,
                file: "motions/haru_g_m15.motion3.json",
            },
            MotionRef {
                group: "TapBody",
                index: 0,
                file: "motions/haru_g_m26.motion3.json",
            },
            MotionRef {
                group: "TapBody",
                index: 1,
                file: "motions/haru_g_m06.motion3.json",
            },
            MotionRef {
                group: "TapBody",
                index: 2,
                file: "motions/haru_g_m20.motion3.json",
            },
            MotionRef {
                group: "TapBody",
                index: 3,
                file: "motions/haru_g_m09.motion3.json",
            },
        ]
    );

    assert_eq!(
        model.eye_blink_parameter_ids(),
        ["ParamEyeLOpen".to_owned(), "ParamEyeROpen".to_owned()]
    );
    assert_eq!(
        model.lip_sync_parameter_ids(),
        ["ParamMouthOpenY".to_owned()]
    );
    assert_eq!(
        model.parameter_group(EYE_BLINK_GROUP).unwrap(),
        ["ParamEyeLOpen".to_owned(), "ParamEyeROpen".to_owned()]
    );
    assert_eq!(
        model.parameter_group(LIP_SYNC_GROUP).unwrap(),
        ["ParamMouthOpenY"]
    );
    assert_eq!(model.parameter_group("Unknown"), None);

    assert_eq!(
        model.hit_area_pairs(),
        [("Head", "HitArea"), ("Body", "HitArea2")]
    );
    assert_eq!(
        model.hit_areas,
        [
            deskwaifu_model_domain::model3::HitArea {
                name: "Head".into(),
                id: "HitArea".into(),
            },
            deskwaifu_model_domain::model3::HitArea {
                name: "Body".into(),
                id: "HitArea2".into(),
            },
        ]
    );
    // Haru declares no Layout.
    assert!(model.layout.is_empty());
}

#[test]
fn tolerates_unknown_keys_at_every_level() {
    let json = r#"{
        "Version": 3,
        "ExtraTopLevel": {"deep": [1, 2, 3]},
        "FileReferences": {
            "Moc": "m.moc3",
            "Textures": ["a.png"],
            "UserData": "m.userdata3.json",
            "Extra": true
        },
        "Groups": [{"Target": "Parameter", "Name": "EyeBlink", "Ids": ["ParamEyeLOpen"], "Note": 1}],
        "HitAreas": [{"Id": "HitArea", "Name": "Head", "Extra": null}],
        "Layout": {"CenterX": 0.0, "CenterY": 0.0, "Width": 2.0}
    }"#;
    let model = Model3::parse(json).unwrap();
    assert_eq!(model.model_file_name(), "m.moc3");
    assert_eq!(model.texture_file_names(), ["a.png"]);
    assert_eq!(model.eye_blink_parameter_ids(), ["ParamEyeLOpen"]);
    assert_eq!(model.hit_area_pairs(), [("Head", "HitArea")]);
    assert_eq!(
        model.layout,
        [
            ("CenterX".to_owned(), 0.0),
            ("CenterY".to_owned(), 0.0),
            ("Width".to_owned(), 2.0),
        ]
        .into_iter()
        .collect()
    );
}

#[test]
fn absent_optional_sections_yield_empty_values() {
    let model = Model3::parse(r#"{"FileReferences":{"Moc":"m.moc3"}}"#).unwrap();
    assert_eq!(model.version, None);
    assert_eq!(model.model_file_name(), "m.moc3");
    assert!(model.texture_file_names().is_empty());
    assert!(model.file_references.physics.is_none());
    assert!(model.file_references.pose.is_none());
    assert!(model.file_references.display_info.is_none());
    assert!(model.expression_names().is_empty());
    assert!(model.file_references.motions.is_empty());
    assert!(model.motion_list().is_empty());
    assert!(model.groups.is_empty());
    assert!(model.eye_blink_parameter_ids().is_empty());
    assert!(model.lip_sync_parameter_ids().is_empty());
    assert!(model.hit_areas.is_empty());
    assert!(model.layout.is_empty());
}

#[test]
fn absent_fade_times_stay_none_instead_of_inventing_defaults() {
    let json = r#"{"FileReferences":{"Moc":"m.moc3","Motions":{
        "Idle":[{"File":"a.motion3.json"}],
        "TapBody":[{"File":"b.motion3.json","FadeInTime":0,"FadeOutTime":1.25}]
    }}}"#;
    let model = Model3::parse(json).unwrap();
    assert_eq!(
        model.file_references.motions[0].motions[0].fade_in_time,
        None
    );
    assert_eq!(
        model.file_references.motions[0].motions[0].fade_out_time,
        None
    );
    assert_eq!(
        model.file_references.motions[1].motions[0].fade_in_time,
        Some(0.0)
    );
    assert_eq!(
        model.file_references.motions[1].motions[0].fade_out_time,
        Some(1.25)
    );
}

#[test]
fn declared_but_empty_optional_file_references_stay_declared() {
    // C++ turns both absent and "" into "", so callers there cannot tell them
    // apart; here an explicit "" is kept and only an absent key is None.
    let model =
        Model3::parse(r#"{"FileReferences":{"Moc":"m.moc3","Physics":""},"Layout":{}}"#).unwrap();
    assert_eq!(model.file_references.physics.as_deref(), Some(""));
    assert!(model.file_references.pose.is_none());
    assert!(model.layout.is_empty());
}

#[test]
fn textures_keep_empty_entries_and_declared_order() {
    let json = r#"{"FileReferences":{"Moc":"m.moc3","Textures":["b.png","","a.png"]}}"#;
    let model = Model3::parse(json).unwrap();
    // SetupTextures() skips "" at load time but still binds by index.
    assert_eq!(model.texture_file_names(), ["b.png", "", "a.png"]);
}

#[test]
fn declaration_order_is_preserved_even_when_group_names_are_unsorted() {
    let json = r#"{"FileReferences":{"Moc":"m.moc3","Motions":{
        "Zeta":[{"File":"z0.motion3.json"},{"File":"z1.motion3.json"}],
        "Alpha":[{"File":"a0.motion3.json"}],
        "Mid":[{"File":"m0.motion3.json"}]
    }}}"#;
    let model = Model3::parse(json).unwrap();
    assert_eq!(
        model.file_references.motions,
        [
            MotionGroup {
                name: "Zeta".into(),
                motions: vec![
                    Motion {
                        file: "z0.motion3.json".into(),
                        fade_in_time: None,
                        fade_out_time: None,
                        sound: None,
                    },
                    Motion {
                        file: "z1.motion3.json".into(),
                        fade_in_time: None,
                        fade_out_time: None,
                        sound: None,
                    },
                ],
            },
            MotionGroup {
                name: "Alpha".into(),
                motions: vec![Motion {
                    file: "a0.motion3.json".into(),
                    fade_in_time: None,
                    fade_out_time: None,
                    sound: None,
                }],
            },
            MotionGroup {
                name: "Mid".into(),
                motions: vec![Motion {
                    file: "m0.motion3.json".into(),
                    fade_in_time: None,
                    fade_out_time: None,
                    sound: None,
                }],
            },
        ]
    );
    assert_eq!(
        model
            .motion_list()
            .iter()
            .map(|motion| (motion.group, motion.index, motion.file))
            .collect::<Vec<_>>(),
        [
            ("Zeta", 0, "z0.motion3.json"),
            ("Zeta", 1, "z1.motion3.json"),
            ("Alpha", 0, "a0.motion3.json"),
            ("Mid", 0, "m0.motion3.json"),
        ]
    );
}

#[test]
fn parsing_is_deterministic() {
    assert_eq!(Model3::parse(HARU).unwrap(), Model3::parse(HARU).unwrap());
    let unsorted =
        r#"{"FileReferences":{"Moc":"m.moc3","Motions":{"B":[{"File":"b"}],"A":[{"File":"a"}]}}}"#;
    let first = Model3::parse(unsorted).unwrap();
    let second = Model3::parse(unsorted).unwrap();
    assert_eq!(first, second);
    assert_eq!(first.motion_list(), second.motion_list());
}

#[test]
fn rejects_duplicate_parameter_group_names() {
    // CubismModelSettingJson silently takes the first match; a duplicate is
    // ambiguous metadata, so the parser rejects it instead.
    let json = r#"{"FileReferences":{"Moc":"m.moc3"},"Groups":[
        {"Target":"Parameter","Name":"LipSync","Ids":["First"]},
        {"Target":"Parameter","Name":"LipSync","Ids":["Second"]}
    ]}"#;
    assert_eq!(
        error(json),
        Model3Error::DuplicateGroup {
            name: "LipSync".into()
        }
    );
    let json = r#"{"FileReferences":{"Moc":"m.moc3"},"Groups":[
        {"Target":"Parameter","Name":"EyeBlink","Ids":["ParamEyeLOpen"]},
        {"Target":"Parameter","Name":"EyeBlink","Ids":["ParamEyeROpen"]}
    ]}"#;
    assert_eq!(
        error(json),
        Model3Error::DuplicateGroup {
            name: "EyeBlink".into()
        }
    );
}

#[test]
fn group_ids_may_be_empty_but_are_required() {
    let model = Model3::parse(
        r#"{"FileReferences":{"Moc":"m.moc3"},"Groups":[{"Target":"Parameter","Name":"LipSync","Ids":[]}]}"#,
    )
    .unwrap();
    assert!(model.lip_sync_parameter_ids().is_empty());
    assert_eq!(
        Model3::parse(
            r#"{"FileReferences":{"Moc":"m.moc3"},"Groups":[{"Target":"Parameter","Name":"LipSync"}]}"#
        ),
        Err(Model3Error::MissingField {
            path: "Groups[0].Ids".into()
        })
    );
}

fn error(json: &str) -> Model3Error {
    Model3::parse(json).unwrap_err()
}

#[test]
fn rejects_malformed_json_and_non_object_documents() {
    assert!(matches!(
        error(r#"{"FileReferences":{"Moc":"m.moc3"}"#),
        Model3Error::InvalidJson(_)
    ));
    assert!(matches!(error("not json"), Model3Error::InvalidJson(_)));
    assert!(matches!(
        error(r#"{"FileReferences":{"Moc":"a.moc3"}} trailing"#),
        Model3Error::InvalidJson(_)
    ));
    assert_eq!(error("[]"), Model3Error::NotAnObject);
    assert_eq!(error("null"), Model3Error::NotAnObject);
    assert_eq!(error(r#""text""#), Model3Error::NotAnObject);
    assert_eq!(error("3"), Model3Error::NotAnObject);
}

#[test]
fn rejects_missing_file_references_or_moc() {
    assert_eq!(
        error("{}"),
        Model3Error::MissingField {
            path: "FileReferences".into()
        }
    );
    assert_eq!(
        error(r#"{"Version":3}"#),
        Model3Error::MissingField {
            path: "FileReferences".into()
        }
    );
    assert_eq!(
        error(r#"{"FileReferences":{}}"#),
        Model3Error::MissingField {
            path: "FileReferences.Moc".into()
        }
    );
    assert_eq!(
        error(r#"{"FileReferences":{"Textures":[]}}"#),
        Model3Error::MissingField {
            path: "FileReferences.Moc".into()
        }
    );
}

#[test]
fn rejects_blank_moc() {
    for moc in ["", "   ", r"\t\n"] {
        assert_eq!(
            error(&format!(r#"{{"FileReferences":{{"Moc":"{moc}"}}}}"#)),
            Model3Error::BlankField {
                path: "FileReferences.Moc".into()
            }
        );
    }
}

#[test]
fn rejects_wrong_types_for_major_fields() {
    assert_eq!(
        error(r#"{"FileReferences":[]}"#),
        Model3Error::WrongType {
            path: "FileReferences".into(),
            expected: "object"
        }
    );
    assert_eq!(
        error(r#"{"FileReferences":{"Moc":3}}"#),
        Model3Error::WrongType {
            path: "FileReferences.Moc".into(),
            expected: "string"
        }
    );
    assert_eq!(
        error(r#"{"FileReferences":{"Moc":"m.moc3","Textures":"a.png"}}"#),
        Model3Error::WrongType {
            path: "FileReferences.Textures".into(),
            expected: "array"
        }
    );
    assert_eq!(
        error(r#"{"FileReferences":{"Moc":"m.moc3","Textures":["a.png",2]}}"#),
        Model3Error::WrongType {
            path: "FileReferences.Textures[1]".into(),
            expected: "string"
        }
    );
    assert_eq!(
        error(r#"{"FileReferences":{"Moc":"m.moc3","Physics":null}}"#),
        Model3Error::WrongType {
            path: "FileReferences.Physics".into(),
            expected: "string"
        }
    );
    assert_eq!(
        error(r#"{"FileReferences":{"Moc":"m.moc3","Expressions":{}}}"#),
        Model3Error::WrongType {
            path: "FileReferences.Expressions".into(),
            expected: "array"
        }
    );
    assert_eq!(
        error(r#"{"FileReferences":{"Moc":"m.moc3","Expressions":[{"Name":"F"}]}}"#),
        Model3Error::MissingField {
            path: "FileReferences.Expressions[0].File".into()
        }
    );
    assert_eq!(
        error(r#"{"FileReferences":{"Moc":"m.moc3","Motions":[]}}"#),
        Model3Error::WrongType {
            path: "FileReferences.Motions".into(),
            expected: "object"
        }
    );
    assert_eq!(
        error(r#"{"FileReferences":{"Moc":"m.moc3","Motions":{"Idle":{}}}}"#),
        Model3Error::WrongType {
            path: "FileReferences.Motions.Idle".into(),
            expected: "array"
        }
    );
    assert_eq!(
        error(r#"{"FileReferences":{"Moc":"m.moc3","Motions":{"Idle":[{"File":1}]}}}"#),
        Model3Error::WrongType {
            path: "FileReferences.Motions.Idle[0].File".into(),
            expected: "string"
        }
    );
    assert_eq!(
        error(
            r#"{"FileReferences":{"Moc":"m.moc3","Motions":{"Idle":[{"File":"a","FadeInTime":"0.5"}]}}}"#
        ),
        Model3Error::WrongType {
            path: "FileReferences.Motions.Idle[0].FadeInTime".into(),
            expected: "number"
        }
    );
    assert_eq!(
        error(r#"{"FileReferences":{"Moc":"m.moc3"},"Groups":{}}"#),
        Model3Error::WrongType {
            path: "Groups".into(),
            expected: "array"
        }
    );
    assert_eq!(
        error(
            r#"{"FileReferences":{"Moc":"m.moc3"},"Groups":[{"Name":"EyeBlink","Ids":"ParamEyeLOpen"}]}"#
        ),
        Model3Error::WrongType {
            path: "Groups[0].Ids".into(),
            expected: "array"
        }
    );
    assert_eq!(
        error(r#"{"FileReferences":{"Moc":"m.moc3"},"Groups":[{"Ids":["a"]}]}"#),
        Model3Error::MissingField {
            path: "Groups[0].Name".into()
        }
    );
    assert_eq!(
        error(r#"{"FileReferences":{"Moc":"m.moc3"},"HitAreas":[{"Name":"Head"}]}"#),
        Model3Error::MissingField {
            path: "HitAreas[0].Id".into()
        }
    );
    assert_eq!(
        error(r#"{"FileReferences":{"Moc":"m.moc3"},"Layout":[]}"#),
        Model3Error::WrongType {
            path: "Layout".into(),
            expected: "object"
        }
    );
    assert_eq!(
        error(r#"{"FileReferences":{"Moc":"m.moc3"},"Layout":{"Width":"2"}}"#),
        Model3Error::WrongType {
            path: "Layout.Width".into(),
            expected: "number"
        }
    );
    // A Layout value that overflows f32 cannot be applied; negative values can.
    assert!(matches!(
        error(r#"{"FileReferences":{"Moc":"m.moc3"},"Layout":{"CenterX":1e39}}"#),
        Model3Error::InvalidNumber { path, .. } if path == "Layout.CenterX"
    ));
    assert_eq!(
        Model3::parse(r#"{"FileReferences":{"Moc":"m.moc3"},"Layout":{"CenterX":-0.25}}"#)
            .unwrap()
            .layout["CenterX"],
        -0.25
    );
    assert_eq!(
        error(r#"{"Version":"3","FileReferences":{"Moc":"m.moc3"}}"#),
        Model3Error::WrongType {
            path: "Version".into(),
            expected: "integer"
        }
    );
    assert_eq!(
        error(r#"{"Version":3.5,"FileReferences":{"Moc":"m.moc3"}}"#),
        Model3Error::InvalidNumber {
            path: "Version".into(),
            value: 3.5
        }
    );
}

#[test]
fn rejects_negative_and_non_finite_fade_times() {
    assert_eq!(
        error(
            r#"{"FileReferences":{"Moc":"m.moc3","Motions":{"Idle":[{"File":"a","FadeInTime":-0.5}]}}}"#
        ),
        Model3Error::InvalidNumber {
            path: "FileReferences.Motions.Idle[0].FadeInTime".into(),
            value: -0.5
        }
    );
    assert_eq!(
        error(
            r#"{"FileReferences":{"Moc":"m.moc3","Motions":{"Idle":[{"File":"a","FadeOutTime":-1}]}}}"#
        ),
        Model3Error::InvalidNumber {
            path: "FileReferences.Motions.Idle[0].FadeOutTime".into(),
            value: -1.0
        }
    );
    // 1e39 overflows f32 to infinity; 1e400 exceeds f64 and serde_json rejects
    // the document before the field is inspected. Both are typed errors.
    assert!(
        matches!(
            error(
                r#"{"FileReferences":{"Moc":"m.moc3","Motions":{"Idle":[{"File":"a","FadeInTime":1e39}]}}}"#
            ),
            Model3Error::InvalidNumber { path, .. } if path == "FileReferences.Motions.Idle[0].FadeInTime"
        ),
        "a fade time that overflows f32 must be rejected"
    );
    assert!(matches!(
        error(
            r#"{"FileReferences":{"Moc":"m.moc3","Motions":{"Idle":[{"File":"a","FadeInTime":1e400}]}}}"#
        ),
        Model3Error::InvalidJson(_)
    ));
    // JSON has no NaN or Infinity literals; they fail as malformed JSON.
    assert!(matches!(
        error(
            r#"{"FileReferences":{"Moc":"m.moc3","Motions":{"Idle":[{"File":"a","FadeOutTime":NaN}]}}}"#
        ),
        Model3Error::InvalidJson(_)
    ));
}

#[test]
fn rejects_duplicate_motion_group_names() {
    assert_eq!(
        error(
            r#"{"FileReferences":{"Moc":"m.moc3","Motions":{"Idle":[{"File":"a"}],"Idle":[{"File":"b"}]}}}"#
        ),
        Model3Error::DuplicateMotionGroup {
            name: "Idle".into()
        }
    );
}

#[test]
fn errors_display_without_panicking() {
    let message = error(r#"{"FileReferences":{}}"#).to_string();
    assert_eq!(message, "missing required field `FileReferences.Moc`");
    assert_eq!(
        Model3Error::DuplicateMotionGroup { name: "X".into() }.to_string(),
        "duplicate motion group `X`"
    );
    assert_eq!(
        Model3Error::DuplicateGroup { name: "Y".into() }.to_string(),
        "duplicate parameter group `Y`"
    );
    assert!(Model3Error::NotAnObject.to_string().contains("object"));
}
