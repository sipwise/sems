# Usage guide

After SEMS executable binary is ready to use (either compiled from sources or installed via packaging — see according guidelines),
it's time to prepare it for a run with specific parameters suiting concrete setup.

Some tips and tricks:
* It is highly recommended to not run SEMS under the root user, as you open up a hole for resources consumption and
  it's just a good manner to always run any daemon under dedicated user/group.
* Manage SEMS via systemd. Systemd gives a versatile approach of managing daemons, which does things (mostly) automatically and eliminates human mistakes,
  if the systemd unit file was previously properly configured. See according chapter of this guide with a systemd unit file example.
* Do not run SEMS constantly in the debug log level mode, because this will overflow your log file and use a lot of disk space.
  SEMS is producing enough log amounts in the debug mode, hence it's recommended to use it carefully.
* To be able to adopt SEMS for your needs and make it a bit more efficient during its run, see according tuning guide.

## Start options

A list of start options available for SEMS binary:
* `-f` - Set configuration file.
* `-x` - Set path for plug-ins.
* `-d` - Set network device (or IP address) for media advertising.
* `-E` - Enable debug mode (do not daemonize, log to stderr).
* `-P` - Set PID file.
* `-u` - Set user ID.
* `-g` - Set group ID.
- `-D` - Set log level (0=error, 1=warning, 2=info, 3=debug; default=2).
- `-v` - Print version.
- `-h` - Print this help.

## sems.conf

Can either be provided by any of the following ways:
- packaging, see the packaging guide
- installed using `make install`, if SEMS is compiled from sources (make sure to have enough permissions on your file system)
- or a simplest way — copy-paste a sample config from the `core/etc/sems.conf.sample` of the cloned repository

To start SEMS with a prepared config:
```
/usr/local/sbin/sems -f <path-to-config>/sems.conf
```

### General sems.conf parameters

Specify whether sems should run in daemon mode (background).
```
fork={yes|no}
```
_fork=no is the same as -E_

Debug mode: do not fork and log to stderr.
```
stderr={yes|no}
```
_stderr=yes is the same as -E_

Sets log level (error=0, warning=1, info=2, debug=3).
```
loglevel={0|1|2|3}
```
_same as -D_

### Plug-in related sems.conf parameters

Sets the path to the plug-ins' binaries:
```
plugin_path=/usr/local/lib/sems/plug-in/
```
_Same as -x_

---

Semicolon-separated list of modules to load. If empty, all modules in plugin_path are loaded.
```
load_plugins=<modules list>
```

Example: 
```
load_plugins=wav;isac;l16;speex;g722;gsm;ilbc;webconference
```

---

semicolon-separated list of modules to exclude from loading. If empty, all modules in plugin_path are loaded.
This has only effect it load_plugins is not set.
```
exclude_plugins=<modules list>
```

Example:
```
exclude_plugins=precoded_announce;py_sems;db_reg_agent;cc_call_timer;cc_ctl;cc_pcalls;cc_prepaid;cc_prepaid_xmlrpc;cc_rest;cc_syslog_cdr
```

---

Using the given path, configuration files of the applications (e.g. dsm.conf) are being looked up.
```
plugin_config_path=/usr/local/etc/sems/etc/
```

### Network related sems.conf parameters

If only one signaling (SIP) and media (RTP) interface is to be used, configure the options:
* `sip_ip`
* `sip_port`
* `media_ip`
* `rtp_low_port`
* `rtp_high_port`
* `public_ip`
* `sig_sock_opts`

If more than one interface is to be used, then use the option 'interfaces' and those per interface options.

---

Parameter: `sip_ip=<ip_address>|<device>`

SIP IP where the SIP stack is bound to. This also sets the value used for the contact header in outgoing calls and registrations.
If neither 'media_ip' nor 'sip_ip' are set, defaults to first non-loopback interface, and the port configured is ignored.

Examples:
```
sip_ip=192.168.0.141
sip_ip=neth1
```

---

Parameter: `sip_port=<port_number>`

Port where its SIP stack should be bound to, ignored if sip_ip not set.
Default: 5060

Example:
```
sip_port=5080
```

---

Parameter: `media_ip=<ip_address>|<device>`

IP address or interface that SEMS uses to send and receive media.
If neither `media_ip` nor `sip_ip` are set, defaults to first non-loopback interface.
However, if `sip_ip` is set, then `media_ip` defaults to `sip_ip`.

Examples:
```
sip_ip=192.168.0.141
sip_ip=neth1
```

---

Parameter: `rtp_low_port=<port>`

Sets lowest for RTP used port. Default: `1024`.

Example:
```
rtp_low_port=10000
```

Parameter: `rtp_high_port=<port>`

Sets highest for RTP used port. Default: `0xffff`.

Example:
```
rtp_high_port=20000
```

---

Parameter: `public_ip=<ip_address>`

Near end NAT traversal. When running SEMS behind certain static NATs, use this parameter to inform SEMS of its public IP address.
If this parameter is set, SEMS will write this value into SDP bodies and Contact.

If this parameter is not set, the local IP address is used.
Important — there is no support for port translation; the local RTP port is advertised in SDP in either case.

---

Parameter: `sig_sock_opts=option,option, ...`

Signaling socket options:
* `force_via_address` — force sending replies to 1st Via.
* `no_transport_in_contact` — do not add transport to contact in replies.

Example:
```
sig_sock_opts=force_via_address
```

---

Parameter: `tcp_connect_timeout=<timeout in millisec>`

Connection timeout for TCP established sessions:
Default: 2000 (2 sec).

Parameter: `tcp_idle_timeout=<timeout in millisec>`

Connection timeout for those hanging TCP sessions with no traffic.

Default: 3600000 (1 hour)

---

Parameter: `interfaces = <list of interface names>`

`interfaces` must be set if more than one interface is to be used for the same purpose (e.g. more than one interface for SIP).
Configure additional interfaces if networks should be bridged or separate networks should be served.

For each interface, a set of parameters suffixed with the interface name should be configured. See examples below.

Please note that for each additional interface, `sip_ip_[if_name]` is mandatory (but can be the interface name, then the first assigned IP is used).
The other parameters are optional.

`media_ip_[if_name]` is derived from `sip_ip_[if_name]` if not set.
`public_ip_[ip_name]` is also based on `sip_ip_[if_name]` if not set explicitly.

Examples:
```
interfaces=intern,extern

sip_ip_intern=eth0
sip_port_intern=5060
media_ip_intern=eth0
rtp_low_port_intern=2000
rtp_high_port_intern=5000

sip_ip_extern=213.192.59.73
sip_port_extern=5060
media_ip_extern=213.192.59.73
rtp_low_port_extern=2000
rtp_high_port_extern=5000
public_ip_extern=213.192.35.73
sig_sock_opts_extern=force_via_address
tcp_connect_timeout_extern=1000
tcp_idle_timeout_extern=900000

```

### NAT related sems.conf parameters

Parameter: `sip_nat_handling={yes|no}`

NAT handling for SIP.
Learn remote next hop address from the source of the address where requests are received from. This option does not apply to the sbc module.

Default: no.

---

Parameter: `force_symmetric_rtp={yes|no}`

Force comedia style "symmetric RTP" NAT handling — learn remote RTP address from where RTP packets come from.
This option does not apply to the sbc module's RTP relay.

Default: no.

### SIP routing related sems.conf parameters

Parameter: `outbound_proxy=<uri>`

This sets an outbound proxy for dialogs and registrations initiated by SEMS.
A preloaded Route header containing the uri is added to each initial request.

The request is then sent to destination obtained by resolving the uri.
If `outbound_proxy` is not set (default setting), no preloaded Route header is added and request is sent to destination obtained by resolving r-uri.

Resolving is done by SIP stack with DNS if uri contains domain name.
Important to remember, if uri can not be resolved, no requests will be sent out at all.

Default: empty (not set).

Example:
```
outbound_proxy=sip:some.domain.com
```

---

Parameter: `force_outbound_proxy={yes|no}`

Forces SEMS to set outbound_proxy for any requests (not just for registrations and dialog initiating requests).
See above what setting of `outbound_proxy` means.

This option will only have an effect if the outbound_proxy option has been set,
and it will break RFC 3261 compatibility in some cases; better to use `next_hop`.

Default: no.

Example:
```
force_outbound_proxy=yes
```

---

Parameter: `next_hop=address[:port]`

If this is set, all outgoing requests will be sent to this address (IP address or domain name), regardless of R-URI etc.

Default: empty (not set).

Examples:
```
next_hop=10.10.0.1
next_hop=yada.yada.com:5065
```

---

Parameter: `next_hop_1st_req={yes|no}`

If set to yes, next_hop behavior (routing without pre-loaded route set) applies only to initial request.
Subsequent requests are routed normally based on route set learned from reply to initial request.

This parameter is preferable for usage because doesn't break SIP based message routing (in-dialog processing).

Default: no.

Example:
```
next_hop_1st_req=yes
```

---

Parameter: `next_hop_for_replies={yes|no}`

Use `next_hop_for_replies` also for SIP replies.

Default: no

Example:
```
next_hop_for_replies=yes
```

### Threads related sems.conf parameters

Parameter: `session_processor_threads=<num_value>`

Controls how many threads should be created that process the application logic and in-dialog signaling.
This is only available if compiled with threadpool support (set `USE_THREADPOOL` in Makefile.defs).

It's recommended to set to `10` if demands of your system aren't high and loading is till around hundreds CPS.

Default: `10`

Example:
```
session_processor_threads=50
```

---

Parameter: `media_processor_threads=<num_value>`

Controls how many threads should be created that process media:
on single-processor systems set this parameter to 1 (default), on multi-processor systems to a higher value.

Recommended value is till `10` if demands of your system aren't high and loading is till around hundreds CPS.
Normally `1` should be more than enough.

Default: `1`

Example:
```
media_processor_threads=10
```

### Application related sems.conf parameters

Parameter: `application`

This controls which application is to be executed for incoming calls (which factory is to be called for buildng a call profile and handler)
if no explicit application requested.

Possibilities:
* `$(ruri.user)` - user part of ruri is taken as application,  e.g. `sip:announcement@host`
* `$(ruri.param)` - uri parameter "app", e.g. `sip:joe@host.net;app=announcement`
* `$(apphdr)` - the value of the P-App-Name header is used
* `$(mapping)` - regex=>application mapping is read from app_mapping.conf (see app_mapping.conf)
* `<application name>` - application name configured here, e.g. application=announcement

Default: empty (not set).

Example:
```
application=$(apphdr)
```

### SIP timers related sems.conf parameters

Parameter: `sip_timer_B=<milliseconds>`
Timer B is the maximum amount of time that a sender will wait for an INVITE message to be acknowledged — i.e. a SIP response message is received.

Default: `64*500`

Example:
```
sip_timer_B=6000
```

---

Parameter: `sip_timer_F=<milliseconds>`
Timer F is the maximum amount of time that a sender will wait for a non-INVITE message to be acknowledged.
SIP messages such as REFER, INFO, MESSAGE, BYE, and CANCEL fall into this category.

Default: `64*500`

Example:
```
sip_timer_F=6000
```

---

Parameter: `sip_timer_G=<milliseconds>`
Initially is set to T1's value. INVITE response retransmission interval.

Default: `500`

Example:
```
sip_timer_G=2000
```

---

Parameter: `sip_timer_t2=<milliseconds>`
Maximum retransmission interval for non-INVITE requests and INVITE responses.

Default: `4000`

Example:
```
sip_timer_G=6000
```

## Systemd unit-file example

This is a working example can be used for managing SEMS via systemd:
```
[Unit]
Description=SEMS B2B
After=mariadb.service
After=network-online.target
Requires=network-online.target
Wants=mariadb.service

[Service]
Type=notify
User=<your-user-here>
Group=<your-group-here>
Environment=LD_LIBRARY_PATH=/usr/local/lib/sems
Environment='CFGFILE=/usr/local/etc/sems/sems.conf'
PassEnvironment=LD_LIBRARY_PATH
PIDFile=/run/sems/sems.pid
Restart=always
ExecStart=/usr/local/sbin/sems -u <your-user-here> -g <your-group-here> -P /run/sems/sems.pid -f $CFGFILE
KillSignal=SIGKILL
SuccessExitStatus=SIGKILL
TimeoutStopSec=10
LimitNOFILE=100000

[Install]
WantedBy=multi-user.target
Alias=sems-b2b.service
```

_It's a serving example! We as an open-source project do not guarantee this will suit your VoIP environment. Make sure to adopt it for your own needs!_