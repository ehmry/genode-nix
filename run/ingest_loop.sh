set -e

finish() {
	ln -s $1 /ingest
	readlink /ingest/$1
}

iterate() {
	local store_root=$(finish $1)
	for (( i=1; i<=8; i++ ))
	do
		name=$1-$i
		ingest_path=/ingest/$name
		mkdir $ingest_path
		cp -r /store/$store_root $ingest_path
		store_root=$(finish $name)
		echo $store_root
	done
}

dd if=/dev/random of=/ingest/random count=8
iterate random

dd if=/dev/zero of=/ingest/zero count=8
iterate zero
