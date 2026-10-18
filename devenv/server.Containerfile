FROM scratch
COPY server /bin/server
COPY libc.so.6 /lib64/libc.so.6
COPY ld-linux-x86-64.so.2 /lib64/ld-linux-x86-64.so.2
ENTRYPOINT ["/bin/server"]
