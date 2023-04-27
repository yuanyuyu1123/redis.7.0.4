redis7.0.4

参考博客：https://blog.csdn.net/qq_45663002/article/details/118931431

编译：

cd deps

make hiredis jemalloc linenoise lua hdr_histogram

cd ../src

make CFLAGS="-march=x86-64"
