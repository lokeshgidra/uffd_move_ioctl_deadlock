$ make
$ ./repro-folio-split-deadlock
Usage: ./repro-folio-split-deadlock <0 no-THP|1 THP> <#threads> <#iters>
$ ./repro-folio-split-deadlock 0 3 5
Address returned by mmap() = from:0x23400000, to:0x7fcbcea00000
iteration 0 completed
iteration 1 completed
iteration 2 completed
iteration 3 completed
iteration 4 completed
$ ./repro-folio-split-deadlock 1 3 5
Address returned by mmap() = from:0x23400000, to:0x7f6d94400000
<stuck!!>
