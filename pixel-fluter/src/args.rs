use clap::{Parser, ValueEnum};

#[derive(Debug, Parser)]
pub struct Args {
    #[clap(short = 's', long)]
    pub pixelflut_sink: String,

    #[clap(short = 't', long, default_value = "2")]
    pub drawing_threads: u16,

    #[clap(short = 'f', long, default_value = "30")]
    pub fps: u32,

    #[clap(long, default_value = "pixelflut")]
    pub shared_memory_name: String,

    #[clap(long, default_value = "binary-sync")]
    pub transmit_mode: TransmitMode,
}

#[derive(Clone, Debug, ValueEnum)]
pub enum TransmitMode {
    // ASCII,
    BinaryPixel,
    BinarySync,
}
