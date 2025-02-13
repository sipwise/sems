# What is SEMS?

[Sipwise](http://www.sipwise.com/) SEMS stands for SIP Express Media Server,
but that's not all it is.

[Sipwise](http://www.sipwise.com/) SEMS is originally a fork of a well known [SEMS open source project](https://github.com/sems-server)
but over the course of time we went far away from it realizing our own ideas and making SEMS project alive and active.
Our project is fully GPL license compatible implementation.

[Sipwise](http://www.sipwise.com/) SEMS primarily is a B2B service,
which is mainly considered to be used in VoIP setups based on SIP protocol signaling.
And still it is quite agnostic in terms of which other software
is to be used in combination with it.

It is also capable of media processing, e.g.:
RTP relay and media generation (based on SEMS provided applications).

It's mostly meant to be used with [Kamailio-](https://github.com/kamailio/kamailio) or
[OpenSIPS-](https://github.com/OpenSIPS/opensips) SIP proxy servers, but also any other
SIP Proxy services supporting RFC3261 and RFC8866 standards.

[Sipwise](http://www.sipwise.com/) SEMS is designed to work based on the event processing model,
which makes it efficient in combination with threading.
It is opposite in this regard to other open-source projects known to the SIP world,
which are traditionally dependent on process fork based implementation
(coming traditionally from the years past) and also are mostly process oriented.


## Mailing List

For general questions, discussion, requests for support, and community chat,
join our [mailing list](https://lists.sipwise.com/mailman/listinfo/). Please do not use
the Github issue tracker for this purpose.

## Features

* full-fledged B2B user agent (its main purpose)
* media processing (RTP relay and media generation)
* transcoding
* custom PBX applications (DSM)
* support of other languages (e.g. Python)
* Redis and MySQL support
* VSC codes handling
* SIP Registration client (based on RPC or configuration file)
* Conferences support (module)
* And many other things! (see `doc/` and `apps/` for more information)

## Documentation

Check our general documentation here:
* [Read-the-Docs](https://sems.readthedocs.io/en/latest/)

## Contribution

Every bit matters. Join us. Make Sipwise SEMS community stronger.
