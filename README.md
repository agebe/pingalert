# pingalert

Tests hosts and services via ping and http and sends SMS notifications via [Notifyre](https://notifyre.com/au) when they go down.

# build 

Install gcc and libcurl and execute make.

# usage

```
Usage: pingalert [OPTION...] 

  -g, --group=<group ID>     the default Notifyre group ID (not the group name,
                             determine via
                             https://api.notifyre.com/addressbook/groups) to
                             use when SMS notifications are send
  -i, --interval=<interval>  check interval in seconds. defaults to 60 seconds
  -k, --notifyre-api-key=<notifyre-api-key>
                             the http header x-api-token to use when sending
                             sms, https://docs.notifyre.com/api/sms-send
  -m, --max-fail=<max-fail>  how many times the service check has to fail
                             before a sms notification is send. defaults to 5
  -n, --notify=<path>        execute program when a service goes up or down
  -p, --prefix=<prefix>      sms prefix that is added to alert and
                             back-to-normal SMS notifications
  -s, --syslog               use syslog logging instead of stdout
  -t, --target=<target>      the target to check, the format is
                             <ping|http|https>://<address>[,[service-name][,[group
                             ID to notify]]]. specify this option multiple
                             times to check multiple targets
  -v, --verbose              enable verbose output
  -w, --warn=<path>          execute program when a service is about to go down
                             (fails test before reaching max-fail)
  -x, --no-sms               do not send out sms, only log and execute
                             specified notify/warn executables
  -?, --help                 Give this help list
      --usage                Give a short usage message
  -V, --version              Print program version
```

# example

```
./pingalert -t http://localhost:8080
```
