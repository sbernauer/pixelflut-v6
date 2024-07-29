use std::time::Duration;

use anyhow::{Context, Result};
use tokio::{io::AsyncWriteExt, net::TcpStream, time};

use tracing::{info, instrument, warn};

use crate::args::TransmitMode;

// We need some marker to detect if the pixel has been set in the meantime. We need to choose a color which is very very
// unlikely to be used by clients. As this marker also contains the alpha channel this is given here.
const UNSET_COLOR_MARKER: u32 = 0x12345678;

#[instrument(skip_all, fields(start_x = start_x, start_y = start_y))]
pub async fn drawing_thread(
    fb_slice: &mut [u32],
    mut sink: TcpStream,
    transmit_mode: TransmitMode,
    fps: u32,
    start_x: u16,
    start_y: u16,
    width: u16,
    height: u16,
) -> Result<()> {
    info!(
        pixels = fb_slice.len(),
        start_x, start_y, "Starting fluting pixels"
    );
    let mut interval = time::interval(Duration::from_micros(1_000_000 / fps as u64));

    loop {
        // let start = std::time::Instant::now();
        match transmit_mode {
            TransmitMode::BinaryPixel => {
                let mut x = start_x;
                let mut y = start_y;

                for rgba in fb_slice.iter_mut() {
                    // Only send pixels that have changed since the last flush
                    if *rgba != UNSET_COLOR_MARKER {
                        let x_bytes = x.to_le_bytes();
                        let y_bytes = y.to_le_bytes();
                        let rgba_bytes = rgba.to_le_bytes();

                        sink.write_all(&[
                            b'P',
                            b'B',
                            x_bytes[0],
                            x_bytes[1],
                            y_bytes[0],
                            y_bytes[1],
                            rgba_bytes[0],
                            rgba_bytes[1],
                            rgba_bytes[2],
                            0, // We are not sending an alpha value as 1.) we don't have one and 2.) even if we had one,
                               // we want to draw our screen 100% to the pixelflut canvas.
                        ])
                        .await
                        .context("Failed to write to Pixelflut sink")?;

                        // Reset color back, so that we don't send the same color twice
                        // *rgba = UNSET_COLOR_MARKER;
                    }

                    x += 1;
                    if x >= width {
                        x = 0;
                        y += 1;
                        if y >= height {
                            warn!(x, y, width, height, "x and y run over the fb bounds. This should not happen, as no thread should get work to do that");
                            // break;
                        }
                    }
                }
            }
            TransmitMode::BinarySync => {
                sink.write_all("PXMULTI".as_bytes())
                    .await
                    .context("Failed to write to Pixelflut sink")?;
                sink.write_u16_le(start_x)
                    .await
                    .context("Failed to write to Pixelflut sink")?;
                sink.write_u16_le(start_y)
                    .await
                    .context("Failed to write to Pixelflut sink")?;
                sink.write_u32_le(
                    fb_slice
                        .len()
                        .try_into()
                        .context("Slice of pixels to send is too large (can be u32::MAX at max). We would need to split the pixels this thread should send further")?,
                ).await.unwrap();
                sink.write_all(u32_to_u8(&fb_slice))
                    .await
                    .context("Failed to write to Pixelflut sink")?;
            }
        };

        // for rgba in fb_slice.iter_mut() {
        //     // Only send pixels that
        //     // 1.) The server is responsible for
        //     // 2.) Have changed since the last flush
        //     if *rgba != UNSET_COLOR_MARKER {
        //         let x_bytes = x.to_le_bytes();
        //         let y_bytes = y.to_le_bytes();
        //         let rgba_bytes = rgba.to_be_bytes();

        //         sink.write_all(&[b'P', b'B']).await?;
        //         sink.write_u16_le(x).await?;
        //         sink.write_u16_le(y).await?;
        //         sink.write_u32(*rgba).await?;
        //         // sink.write_all(&[
        //         //     b'P',
        //         //     b'B',
        //         //     x_bytes[0],
        //         //     x_bytes[1],
        //         //     y_bytes[0],
        //         //     y_bytes[1],
        //         //     rgba_bytes[0],
        //         //     rgba_bytes[1],
        //         //     rgba_bytes[2],
        //         //     rgba_bytes[3],
        //         // ])
        //         // .await
        //         // .context("Failed to write to Pixelflut sink")?;

        //         // // Ignore alpha channel
        //         // let rgb = *rgba >> 8;
        //         // sink.write_all(format!("PX {x} {y} {rgb:06x}\n").as_bytes())
        //         //     .await
        //         //     .context("Failed to write to Pixelflut sink")?;

        //         // Reset color back, so that we don't send the same color twice
        //         // *rgba = UNSET_COLOR_MARKER;
        //     }

        // x += 1;
        // if x >= width {
        //     x = 0;
        //     y += 1;
        //     if y >= height {
        //         warn!(x, y, width, height, "x and y run over the fb bounds. This should not happen, as no thread should get work to do that");
        //         // break;
        //     }
        // }
        // }

        // let elapsed = start.elapsed();
        // info!(
        //     ?elapsed,
        //     duty_cycle =
        //         (elapsed.as_micros() as f32 / interval.period().as_micros() as f32 * 100.0).ceil(),
        //     "Loop completed",
        // );

        sink.flush()
            .await
            .context("Failed to flush to Pixelflut sink")?;

        interval.tick().await;
    }
}

// Thanks to https://users.rust-lang.org/t/transmute-u32-to-u8/63937/2
fn u32_to_u8(arr: &[u32]) -> &[u8] {
    let len = 4 * arr.len();
    let ptr = arr.as_ptr() as *const u8;
    unsafe { std::slice::from_raw_parts(ptr, len) }
}
