# The lab's OpenSSH server (#1871): a pinned Alpine base plus the distribution's openssh. Users, keys,
# host keys and configuration arrive at run time from the private state folder.
FROM docker.io/library/alpine@sha256:f59abe656b692d901e184be3445e084786602108599d2a51d229c0f5a48c71bf
RUN apk add --no-cache openssh-server openssh-sftp-server && rm -f /etc/ssh/ssh_host_*_key*
COPY sftp-entrypoint.sh /usr/local/bin/entrypoint
ENTRYPOINT ["/usr/local/bin/entrypoint"]
