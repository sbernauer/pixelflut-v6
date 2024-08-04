import math
import ipaddress

WIDTH = 1920
HEIGHT = 1080
NUM_SERVERS = 16

assert NUM_SERVERS < WIDTH, "For this script there must be less servers than the screen width! In theory you can have more servers (up to one server per pixel), but this script was build such that servers get whole vertical slices"

NETWORK = ipaddress.ip_network("2000:42::/64")
assert NETWORK.prefixlen == 64, "NETWORK needs to be /64!"

subnet_bits = math.log2(NUM_SERVERS)
assert subnet_bits.is_integer(), "NUM_SERVERS need to be a power of two!"
subnets = list(NETWORK.subnets(prefixlen_diff=int(subnet_bits)))
assert len(subnets) == NUM_SERVERS

chunk_width = WIDTH / NUM_SERVERS
assert chunk_width.is_integer(), "The screen width needs to be a multiple of the resulting chunk width of {chunk_width}. This script was build such that servers get whole vertical slices"
chunk_width = int(chunk_width)

for server in range(NUM_SERVERS):
    subnet = NETWORK
    startX = server * chunk_width
    endX = (server + 1) * chunk_width - 1
    print(f"Server {server} has subnet {subnets[server]} with x ranging from {startX} to {endX}")
