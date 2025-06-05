use std::slice;

use anyhow::{Context, Result, bail};
use args::Args;
use clap::Parser;
use drawer::Drawer;
use prometheus_exporter::PrometheusExporter;
use shared_memory::ShmemConf;
use tokio::net::TcpStream;
use tracing::{debug, info, warn};

use crate::{statistics::Statistics, ui::Ui};

mod args;
mod drawer;
mod prometheus_exporter;
mod statistics;
mod ui;

// Width and height, both of type u16.
const HEADER_SIZE: usize = 2 * std::mem::size_of::<u16>();

/// This needs to align with the `MAX_PORTS` constant in the server code, so that we end up with the same memory layout
/// for the shared memory!
pub const MAX_PORTS: usize = 32;

#[tokio::main]
async fn main() -> Result<(), anyhow::Error> {
    let args = Args::parse();

    tracing_subscriber::fmt().init();

    let shared_memory = ShmemConf::new()
        .os_id(&args.shared_memory_name)
        .open()
        .with_context(|| {
            format!(
                "Failed to open shared memory with the OS id \"{}\" (probably at location /dev/shm/{}). Is the backend running? Are you missing permissions (try sudo)?",
                args.shared_memory_name, args.shared_memory_name
            )
        })?;

    debug!(size = shared_memory.len(), "Loaded shared memory");
    if shared_memory.len() < HEADER_SIZE {
        bail!(
            "Invalid shared memory length. It needs to have at least a length of {HEADER_SIZE} bytes for the header,
                but it only has {} bytes.",
                shared_memory.len());
    }
    let size_ptr = shared_memory.as_ptr() as *const u16;
    let width = unsafe { *size_ptr };
    let height = unsafe { *size_ptr.add(1) };

    if width == 0 || height == 0 {
        bail!(
            "The size of the framebuffer was ({width}, {height}). Both values need to be non-null"
        );
    }
    info!(width, height, "Found existing framebuffer");

    warn!(
        width,
        height,
        "I should ask the server what resolution it is using (using the SIZE command) and check if they are the same, \
        but I'm lazy. Until this is implemented, it is your responsibility to make sure the resolutions match"
    );

    let fb: &mut [u32] = unsafe {
        slice::from_raw_parts_mut(
            shared_memory.as_ptr().add(4) as _,
            width as usize * height as usize,
        )
    };

    let current_statistics: &Statistics = unsafe {
        (shared_memory
            .as_ptr()
            .add(4 /* header */)
            .add(width as usize * height as usize * 4 /* pixels */) as *const Statistics)
            .as_ref()
            .unwrap()
    };

    let sink = TcpStream::connect(&args.pixelflut_sink)
        .await
        .with_context(|| {
            format!(
                "Failed to connect to Pixelflut sink at {}",
                &args.pixelflut_sink
            )
        })?;
    let mut drawer =
        Drawer::new(fb, sink, width, height, &args).context("Failed to created drawer")?;
    tokio::spawn(async move {
        drawer.run().await.expect("failed to run drawer");
    });

    let prometheus_exporter = PrometheusExporter::new(current_statistics)
        .context("Failed tio start Prometheus exporter")?;
    tokio::spawn(async move { prometheus_exporter.run().await });

    let mut ui = Ui::new(current_statistics);
    ui.start().context("Failed to start UI")
}
