use std::{env, error::Error, fs::File, io::BufWriter, path::PathBuf};

use cubism_bridge::{Runtime, rgba_len, unpremultiply_rgba};

fn main() -> Result<(), Box<dyn Error>> {
    let mut args = env::args_os().skip(1);
    let usage = "usage: offscreen MODEL.model3.json OUTPUT.png [MOTION_GROUP INDEX FRAMES]";
    let model_path = PathBuf::from(args.next().ok_or(usage)?);
    let output = PathBuf::from(args.next().ok_or(usage)?);
    let motion = match args.next() {
        Some(group) => {
            let group = group.into_string().map_err(|_| usage)?;
            let index = args
                .next()
                .ok_or(usage)?
                .into_string()
                .map_err(|_| usage)?
                .parse()?;
            let frames: u32 = args
                .next()
                .ok_or(usage)?
                .into_string()
                .map_err(|_| usage)?
                .parse()?;
            if frames == 0 || frames > 3600 || args.next().is_some() {
                return Err(usage.into());
            }
            Some((group, index, frames))
        }
        None => None,
    };
    const WIDTH: u32 = 640;
    const HEIGHT: u32 = 640;
    let runtime = Runtime::new(WIDTH, HEIGHT)?;
    eprintln!("OpenGL renderer: {}", runtime.renderer());
    let mut model = runtime.load(&model_path)?;
    eprintln!("Cubism masking: {}", model.uses_masking());
    let mut pixels = vec![0; rgba_len(WIDTH, HEIGHT).expect("valid example dimensions")];
    if let Some((group, index, frames)) = motion {
        model.play_motion(&group, index)?;
        for _ in 0..frames {
            model.render(1.0 / 60.0, &mut pixels)?;
        }
    } else {
        model.render(0.0, &mut pixels)?;
    }
    if !pixels.as_chunks::<4>().0.iter().any(|pixel| pixel[3] != 0) {
        return Err("Cubism produced an entirely transparent frame".into());
    }
    unpremultiply_rgba(pixels.as_chunks_mut::<4>().0);
    let mut encoder = png::Encoder::new(BufWriter::new(File::create(output)?), WIDTH, HEIGHT);
    encoder.set_color(png::ColorType::Rgba);
    encoder.set_depth(png::BitDepth::Eight);
    encoder.write_header()?.write_image_data(&pixels)?;
    Ok(())
}
