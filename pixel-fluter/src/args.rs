use clap::{Parser, ValueEnum};

#[derive(Debug, Parser)]
pub struct Args {
    #[clap(short = 's', long)]
    pub pixelflut_sink: String,

    #[clap(short = 'f', long, default_value = "30")]
    pub fps: u16,

    #[clap(long, default_value = "pixelflut")]
    pub shared_memory_name: String,

    #[clap(long, default_value = "binary-sync")]
    pub transmit_mode: TransmitMode,

    /// Shard the x coordinates into the given number of slices.
    #[clap(long, default_value = "1")]
    pub x_shards: u16,

    /// Only draw the specified shard.
    #[clap(long, default_value = "1")]
    pub x_shard: u16,
}

#[derive(Clone, Debug, ValueEnum)]
pub enum TransmitMode {
    // Not implemented yet
    // ASCII,
    // BinaryPixel,
    BinarySync,
}
