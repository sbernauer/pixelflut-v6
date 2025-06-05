use std::time::Duration;

use anyhow::{Context, ensure};
use tokio::{io::AsyncWriteExt, net::TcpStream, time::interval};

use crate::args::{Args, TransmitMode};

pub struct Drawer<'a> {
    fb_slice: &'a mut [u32],
    sink: TcpStream,

    width: u16,
    height: u16,

    fps: u16,
    // threads: u16,
    transmit_mode: TransmitMode,
    x_shard: u16,
    x_shard_width: u16,
}

impl<'a> Drawer<'a> {
    pub fn new(
        fb_slice: &'a mut [u32],
        sink: TcpStream,
        width: u16,
        height: u16,
        args: &Args,
    ) -> anyhow::Result<Self> {
        let (x_shard, x_shards) = (args.x_shard, args.x_shards);
        ensure!(
            x_shard <= x_shards,
            "The X shard number {x_shard} can not be greater than the number of shards {x_shards}"
        );
        ensure!(
            width % x_shards == 0,
            "The width {width} must be divisible by the number of X shards {x_shards}"
        );

        Ok(Self {
            fb_slice,
            sink,
            width,
            height,
            fps: args.fps,
            // threads: args.drawing_threads,
            transmit_mode: args.transmit_mode.clone(),
            x_shard: args.x_shard,
            x_shard_width: width / args.x_shards,
        })
    }

    pub async fn run(&mut self) -> anyhow::Result<()> {
        let mut interval = interval(Duration::from_micros(1_000_000 / self.fps as u64));

        loop {
            interval.tick().await;
            self.draw().await?;
        }
    }

    /// Draws line by line
    async fn draw(&mut self) -> anyhow::Result<()> {
        // shards start at 1, pixels start at 0.
        let start_x = self.x_shard_width * (self.x_shard - 1);
        let end_x = start_x + self.x_shard_width;

        for y in 0..self.height {
            self.draw_line(y, start_x, end_x).await?;
        }

        self.sink.flush().await.context("Failed to flush sink")?;

        Ok(())
    }

    async fn draw_line(&mut self, y: u16, start_x: u16, end_x: u16) -> anyhow::Result<()> {
        match self.transmit_mode {
            TransmitMode::BinarySync => {
                let to_draw = &self.fb_slice[y as usize * self.width as usize + start_x as usize
                    ..y as usize * self.width as usize + end_x as usize];
                assert_eq!(to_draw.len(), end_x as usize - start_x as usize);

                self.sink
                    .write_all("PXMULTI".as_bytes())
                    .await
                    .context("Failed to write to Pixelflut sink")?;
                self.sink
                    .write_u16_le(start_x)
                    .await
                    .context("Failed to write to Pixelflut sink")?;
                self.sink
                    .write_u16_le(y)
                    .await
                    .context("Failed to write to Pixelflut sink")?;
                self.sink
                    .write_u32_le(
                        to_draw
                            .len()
                            .try_into()
                            .context("Pixels to draw did not fit in u32")?,
                    )
                    .await
                    .context("Failed to write to Pixelflut sink")?;
                self.sink
                    .write_all(u32_to_u8(to_draw))
                    .await
                    .context("Failed to write to Pixelflut sink")?;
            }
        }

        Ok(())
    }
}

// Thanks to https://users.rust-lang.org/t/transmute-u32-to-u8/63937/2
fn u32_to_u8(arr: &[u32]) -> &[u8] {
    let len = 4 * arr.len();
    let ptr = arr.as_ptr() as *const u8;
    unsafe { std::slice::from_raw_parts(ptr, len) }
}
