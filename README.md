# pixelflut-v6

This README starts with installation and running instructions.
To get background information on the history of pixelflut as well as design aspects of the "new" pixelflut v6/pingxelflut protocol please read on [History](#history) and [Design aspects](#design-aspects).

## Building

TODO

## Running

TODO

## History

> **_NOTE:_** The section might be partially wrong, as I might missed certain dates or implementations, but I hope the overarching concepts are correct.
> I'm very happy about pointing out wrong/missing facts and PRs!

### ASCII Pixelflut (2012)

The oldest mention of Pixelflut I could find was in the repo https://github.com/defnull/pixelflut dating back to 2012.
It's the original protocol sending ASCII commands over TCP in the form of `PX 123 4567 ffffff`, so let's call it ASCII Pixelflut.

There are some really fast implementations written in C and Rust (e.g. shoreline, breakwater and others).
At GPN22 we reached 130 Gbit/s using real clients (so no loopback involved) on an dual socket AMD EPYC system with 128(?) cores using breakwater.

The problem with this protocol is that the clients have an very easy job: In case you are fluting a static image, just calculate the `PX` commands in memory once and just `memcp` them into the TCP socket in a loop.
This is computationally cheap, so my Laptop can generate the 130 Gbit/s of traffic mentioned above.
However, the server needs to actually parse the ASCII commands the clients are sending, which is computationally much more expensive and also have the needed memory bandwidth to store the pixel framebuffer.

This highlights the following problems of the protocol:

1. On Laptop can saturate the biggest server we could get
2. Not even can a single client easily saturate the server, there are multiple of them hitting a single server!
   The problem can not be easily distributed over multiple servers, as every TCP packet can contain multiple `PX` commands for all possible pixels on the screen.
   Because of this you can not easily shard the traffic by e.g. screen areas without the need that every server needs to parse all traffic and forward `PX` commands to the correct server.
   This get's especially hard once you want to handle read requests.
3. The format is as easy as possible for clients, but actual parsing is required on the server side due to the fact that x and y coordinates can be 1 to 4 characters long.
   Try to write an efficient SIMD code to parse the PX commands and you will probably understand my point ;)
This results in the fact that 99% of the time the server is the bottleneck on events.
Either because it does not have more network connectivity or because it is too slow.
This results in clients not needing to optimize - you get e.g. only 10 TCP connections and once the server reached it's maximum speed there is no point in further optimizing your client - your client's performance doesn't matter any more.
Small side note: That's one of the reasons why I have never written any meaningful client but mostly focused on writing fast server implementations ;)

### Binary Pixelflut (2022)

To address the problem 3.) above a special binary command `PB` was introduced in [this commit](https://github.com/timvisee/pixelpwnr-server/commit/e22983a64b9fc5e025cde7b21f574e5881797c62), which is documented [here](https://github.com/timvisee/pixelpwnr-server?tab=readme-ov-file#the-binary-px-command).
It has the format `PBxyrgba` and encodes x, y and rgba as bytes directly instead of converting them to variable length ASCII texts.
That has the big benefit that the server does not need to do any parsing, but can take the bytes following `PB` and directly use them.

### pixelflut v6 (2017)

> **_NOTE:_** I'm calling this pixelflut v6 rather than pingxelflut, to leave this name to the next item on this list

At GPN17 a completely new [protocol](https://entropia.de/GPN17:Pingxelflut) was invented.
It does not use TCP any more, instead one network packet colors one pixel.
One funny thing is that the x, y and rgba are *not* encoded in the IP packet's payload, but in the IPv6 address instead.

It has the following benefits in comparison to the ASCII Pixelflut:

1. Very easy to parse format
2. Normal operating systems (such as the Linux kernel) are limited by how many packets/s they can process.
   This means all traffic levels are really slow when you are writing a good old userspace program.
   My hope is that we spend the effort of using some magic on the server to avoid this limitation and most clients still using the kernel and being limit by their own CPU - which would be great as it would motivate clients to optimize.
3. Multiple servers can be used to distribute the load across.
   This is achieved by not routing an /64 to a single server, but by splitting the /64 into two /65 and routing them to two different servers.
   As the 65th bit is the first bit of the x coordinate, the first servers handles left side of the screen and the second server handles the right side of the screen.
   This concept can be used to distribute the load to any power of two number of servers (at a maximum having a dedicated server for each pixel ^^).

### pingxelflut (2024)

At GPN22 some people gathered to design a new protocol based purely on ICMP messages in https://github.com/kleinesfilmroellchen/pingxelflut/.
The protocol encodes the command ("set the pixel") as well as the x, y and rgba binary within the ICMP payload and is therefore easy to parse by the server.

It's pretty similar to the pixelflut v6 protocol, just a little bit less crazy (as payload is payload) ;) and has the downside of not being able to shard the traffic across multiple servers.

### Revamp of pixelflut v6 (2024)

I was very happy with the state of my pixelflut v6 implementation back in 2020, but everyone told me that there is no way of running such crazy packet rates at events such as the GPN or congress, as it would cause problems with the network infrastructure.
So I closed the chapter as a very interesting thing I heave learned a lot from, but as not being used in real life.
However, my interested was raised again, after seeing the Pixelflut setup using dedicated switches at GPN22 to do interfere with regular traffic.

The result of this is this repository :)

## Design aspects

TODO
