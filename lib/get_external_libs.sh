#!/bin/sh

EXTERNAL_HEADERS_DIR=headers
ASYNCIO_REPO_URL=http://github.com/ernacktob/asyncio
ASYNCIO_SRC=asyncio
ASYNCIO_GIT=$ASYNCIO_SRC/.git

if [ ! -d $ASYNCIO_GIT ]
then
	git clone $ASYNCIO_REPO_URL
	cd $ASYNCIO_SRC
else
	cd $ASYNCIO_SRC
	git pull
fi

make
cd ..
cp $ASYNCIO_SRC/libasyncio.* .
cp $ASYNCIO_SRC/asyncio.h $EXTERNAL_HEADERS_DIR
