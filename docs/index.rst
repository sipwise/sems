.. _SEMS: https://github.com/sipwise/sems
.. _rtpengine: https://github.com/sipwise/rtpengine
.. _Sipwise: https://www.sipwise.com
.. _Kamailio: https://www.kamailio.org/

Welcome to the SEMS Project Documentation
==============================================

Sipwise SEMS is a drop-in replacement for B2B (SIP/media) components.

Sipwise SEMS is originally a fork of a well known SEMS open source project
but over the course of time we went far away from it realizing our own ideas and making SEMS project alive and active.
Our project is fully GPL license compatible implementation.

Sipwise SEMS primarily is a B2B service, which is mainly considered to be used in VoIP setups based on SIP protocol signaling.
And still it is quite agnostic in terms of which other software is to be used in combination with it (Kamailio, OpenSIPS etc.).

It is capable of media processing, e.g.: RTP relay and media generation (based on SEMS provided applications).

It’s mostly meant to be used with Kamailio- or OpenSIPS- SIP proxy servers, but also any other SIP Proxy services supporting RFC3261 and RFC8866 standards.

Sipwise SEMS is designed to work based on the event processing model, which makes it efficient in combination with threading.
It is opposite in this regard to many other open-source projects known to the SIP world,
which are traditionally dependent on process fork based implementation (coming traditionally from the years past) and also are mostly process oriented.

For more information see "Overview" chapter of this guide.

==============================================

.. toctree::
   :maxdepth: 4

   Overview <overview>

==============================================

.. toctree::
   :maxdepth: 1

   Usage <usage>
   Compilation and installation <installation>
   Packaging <packaging>
