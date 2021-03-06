# OS Project 4 - Redundancy Detection
### Noah Yoshida, Chan Hee Song, Nick Marcopoli, Allen Duong

## Usage

Use

`$ make`

to compile the program `threadedRE`.

Run the program

`$ ./threadedRE -t [CONSUMER_THREADS] -l [LEVEL] FILE_1 FILE_2 .... `

if no options are given, the level defaults to 1 and the number of consumer
threads defaults to 1 (the optimal number).

If for some reason you want to run the debug mode of the file, compile using

`$ make debug`

and run the program like normal. Warning: there will be a lot of output.

If you want to run the test scripts, showing the times for different thread
configurations (over the medium sizef pcap file), run

`$ make test`

## Implementation

In accordance with the project guidelines, we are using a producer / consumer
thread setup with condition variables to signal states, and locks to guarantee
thread safety. We have a single producer that loops through the input file and
parses out the packets - which it puts in the shared buffer. Our program's
concept of a packet is a simple struct that holds the data and the size of the
packet. The shared buffer is an array of pointers to these structs, as the
packet structs are
dynamically allocated on the heap (to ensure the data is persisted between
threads). We found that the buffer is almost never full, and having a buffer of
size 15 is sufficient in most cases.

This program is I/O bound in its performance, meaning that the slowest part of
it is more or less reading the packets from the input file(s). Unfortunately, due to the
irregular nature of the packet sizes in the file, we cannot chunk it up and have
multiple producers concurrently parsing the file. Instead, we have multiple
consumers who read from the queue and either accept or reject the incoming
packets. In practicality, due to this nature of the problem, we found that only
having one consumer thread is optimal. The overhead in thread creation is
significant enough to be noticeable, and since the program is limited by the
speed at which we can read in the data, having additional consumers does not
help much, as long as the buffer is long enough to hold a few items in it in
case the consumer does not get scheduled right after the producer finishes.

We determined that this was the bottleneck when profiling the program using the
tool callgrind and viewing the results with KCacheGrind. It is also easy to see
this as the time spent in kernel mode is longer than the time spent in user mode
(seen from the output of the `$ time` command).

Our method for detecting redundancy is to use a **Bloom Filter**. When the
consumer reads a packet from the buffer, it hashes the data several times and then
tries to add the data to the bloom filter by setting the bits at the indicies
specified by the hashes to 1. By default it assumes the packet is
redundant, but if there are any indicies with values of 0 where it it accessing,
it will know that this is not a redundant packet. After adding the data to the
bloom filter, we can free the packet from memory, as all the data about the
packet has been encoded into the bloom filter.

More information about bloom filters can be found here:

https://en.wikipedia.org/wiki/Bloom_filter

While we initially were using a hash table, we noticed that the accuracy of the program
greatly decreased the more data we read in. Since there was a strict 64 MB
limit, saving the packet data seemed like a waste, and with a bloom filter we
are able to store much more information in a smaller space. While the bloom
filter in theory should have some false potivies, these will only be strictly worse at
files with data under 64 MB. These false positives can also be greatly mitigated
by using good hashing functions. Our measured false positive rate seems to be
very low - under 1% for level 1 given the largest file. This is because our bloom filter can be made as
large as the maximum data constraint, or 64 MB. Each index in the bloom filter
is 1 bit long, and thus we can have around 510 million indicies in it
(~64,000,000 bytes * 8 = 510,000,000 bits).

Once we start getting to larger collections of
data, the bloom filter really starts to shine. We have been able to process very
large combinations of files with very little accuracy degradation which would be
present in a cache that requires data eviction.

When processing high amounts of original data, the bloom filter can fill up.
Accuracy degrades when it gets around 1/3 full, as it will start to frequently
report false positives. We have decided to forefully
evict everything from it when that happens. Even though this seems extreme, we
are able to hold out for much longer and keep more data in memory than an
implementation using traditional hash tables and saving the packet data, and
thus the accuracy of this should be strictly better on average.

Our hashing functions are the **djb2** has and the **murmur3** hash. While just
using two hashing functions with a bloom filter would not be sufficient in
guaranteeing a good spread of hits in the filter, we use the Kirsch-Mitzenmacher-Optimization
to use the djb2 and murmur3 hashes multiple times. Details about this
optimization can be found here:

https://www.eecs.harvard.edu/~michaelm/postscripts/tr-02-05.pdf

Another advantage of the bloom filter is that after we hash the packet data, we
never have to loop through it again. While calculating two hashes takes more
time, we end up coming out about even when we take into the account the linear
operations we eliminate from other parts of the program. It also makes the code
much simpler, since we do not have to keep the packets around after consuming
them, and don't have to worry about evicting data.

Throughout the code we use a few macros. `ERROR` and `check` in particular.
`ERROR` prints the line it was on and exits with 1. `check` checks the return
value of whatever it is wrapping, and if it is not valid (< 0), it calls
`ERROR`.

The C implementation of the `murmur3` has was found here

https://github.com/PeterScott/murmur3

and our djb2 hash implementation is original.

Several tools were used in testing the program, particularly KCacheGrind and the
valgrind tools cachegrind and callgrind.
