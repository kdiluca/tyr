#!/bin/bash
set -e

if [ -n "$1" ] && [ -d "$1" ]; then
	pushd "$1"
else
	pushd .
fi

# grab the latest zmq library:
rm -rf libzmq
git clone --depth=1 --recurse-submodules --single-branch --branch=master https://github.com/zeromq/libzmq.git
pushd libzmq
./autogen.sh
./configure --without-libsodium --without-documentation
make -j4
sudo make install
popd

# grab experimental zmq-based server API:
rm -rf prime_server
git clone --depth=1 --recurse-submodules --single-branch --branch=master https://github.com/kevinkreiser/prime_server.git
pushd prime_server
./autogen.sh
./configure
make -j4
sudo make install
popd

popd
