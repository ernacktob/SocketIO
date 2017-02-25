#!/bin/sh

EXTERNAL_HEADERS_DIR=headers
ASYNCIO_REPO_URL=http://github.com/ernacktob/asyncio
ASYNCIO_SRC=asyncio
ASYNCIO_GIT=$ASYNCIO_SRC/.git
changed=0

mkdir -p $EXTERNAL_HEADERS_DIR

if [ ! -d $ASYNCIO_GIT ]
then
	git clone $ASYNCIO_REPO_URL && changed=1
	cd $ASYNCIO_SRC
else
	cd $ASYNCIO_SRC
	git pull | grep -q -v 'Already up-to-date.' && changed=1
fi

make
cd ..

if [ ! -f $EXTERNAL_HEADERS_DIR/asyncio.h ]
then
	cp $ASYNCIO_SRC/asyncio.h $EXTERNAL_HEADERS_DIR
else
	if [ $changed -eq "1" ]
	then
		cp $ASYNCIO_SRC/asyncio.h $EXTERNAL_HEADERS_DIR
	fi
fi

if [ ! -f libasyncio.* ]
then
	cp $ASYNCIO_SRC/libasyncio.* .
else
	if [ $changed -eq "1" ]
	then
		cp $ASYNCIO_SRC/asyncio.h $EXTERNAL_HEADERS_DIR
	fi
fi
