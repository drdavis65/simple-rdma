# SDR-RDMA API
The directory contains SDR SDK API headers (`sdr.h`, `sdr_mr.h`) and a bulky RDMA write benchmark.
The headers contain SDR QP API definitions as presented in the paper.

# SDR Write Benchmark

The file `sdr_write_bw.c` contains an RC `ib_write_bw`-like benchmark. The `./common/` directory contains non-SDR API utilities.
The sender side uses bulky Write to inject a window of buffers.
An iteration of the benchmark completes when all chunks of the receive message are received (all bits in the bitmap are fired up).