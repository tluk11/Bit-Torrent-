1. Supported Features 
● Download: Parse .torrent files, contact tracker, multi-peer downloading, SHA-1 piece verification,\n 
persistent disk storage \n 
● Upload: Listen for connections, send bitfield, implement choking/unchoking, respond to PIECE \n 
requests \n 
● Protocol: BitTorrent handshake, message parsing, length-prefixed framing, peer state machine\n 
\n 
2. Design and implementation choices \n 
● Modular design with separate components for download, upload, and peer management \n 
● We decided to create a bit torrent that had two different states while open, either downloading \n 
or uploading. It was really difficult to implement Bittorrent to send pieces while also \n 
downloading them from peers. So we just made it so that it would turn into the seeding state \n 
after completely downloading the torrent file. \n 
● Data Structures: PieceBuffer array stores in-memory pieces during download/seeding; Peer \n 
tracks connection state and choking status \n 
● Key Decisions: Keep pieces in RAM for seeding speed; use select() for non-blocking I/O; enforce \n 
16KB block size limit; max 4 concurrent unchoked peers \n 
● Implemented peer mode which allows a client to download exclusively from a peer (passed into \n 
arguments)\n 
\n 
3. Testing/measurements/experiments, especially those distinct from \n 
the demo. \n 
● We tested each part of our code by using a .torrent file and then checking that it correctly \n 
worked the way we implemented. \n 
● For example: when we made the parser for the torrent file, we had a struct that stored the info \n 
from it, and we tested this implementation by outputting what the struct contained and seeing if \n 
it matched to what we expected. \n 
● We added a lot of log messages and printf statements to understand where in the process we \n 
were failing \n 
● We used test files/scripts to test various modules of our client such as contacting the tracker or \n 
seeing if our client was accepting new connections in the seeding phase. \n 
● To measure our download speed, we would store the file size as well as how long the download \n 
took and calculate our average download speed (not displayed but for testing purposes).\n 
\n 
4. Problems encountered (and if/how addressed) \n 
● The torrent was really slow. We realized it was because we coded our client to iterate \n 
synchronously through the handshakes and then started requesting pieces from peers one at a \n 
time. In order to make it faster, we started requesting multiple pieces at once to make it faster, \n 
which worked. \n 
● Another problem we encountered was that we were really struggling to get the uploading part \n 
of Bittorrent client to work. For some reason it always disconnected from peers right after \n 
completing the download and wouldn’t be able to receive anything from any new peers who \n 
would try to contact our client. We did multiple tests including using wireshark to see the error. \n 
So far we were unable to figure out the issue.  \n 
● Another problem during the upload phase was that there would be random mallocs or \n 
segmentation faults. To resolve this, we used gdb and valgrind thoroughly to find where our \n 
program was crashing and to see if we had any memory leaks.\n 
\n 
5. Known bugs or issues \n 
● Common issues that we ran into were not being able to tell how fast our Bittorrent was at \n 
downloading, this was because the peer list for the torrent file varied at times and we were \n 
unable to get a consistent reading if our bittorrent was fast enough \n 
● An issue we believe is occurring is that we are unable to be put on the tracker’s peer list due to \n 
the port not being seen on the internet. We could not figure out how to get it visible, we suspect \n 
it is something to do with WSL or the way docker hides addresses \n 
● If you wait too long after being prompted to enter the seeding phase, the program may exit or \n 
malloc. We don’t really know why, but it is consistent. \n 


