# Overview

Threadpool is a small header-only threadpool implementation. It spawns a certain number of threads and executes tasks in parallel.

## Features

Unlike other implementations, threadpool does not use a container (such as `std::vector<std::thread>`). Instead, it spawns a certain number of threads, calls the `detach()` method and communicates with the threads using condition variables.
As a result, it is possible to resize the pool (by either adding or deleting threads) at runtime.

## Usage

An example is provided to illustrate its usage. To run the example, do the following (assuming that the sources are located under `~/threadpool`):

```shell
$ cd ~/threadpool
$ mkdir build && cd build && cmake .. && make
$ cd ../bin
$ ./threadpool
```
