FROM scratch
COPY client /bin/client
ENTRYPOINT ["/bin/client"]
CMD ["idle"]
