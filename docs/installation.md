# Compilation and installation

## Compile from sources

This guide instructs you how to manually compile SEMS from sources and prepare it for further usage.

First of all get the latest version of SEMS from the GH repository with the command:
```
git clone https://github.com/sipwise/sems.git
```

_If you still don't have git tool in your environment, just install `git` package from an official repository of your distribution._

Now, it may happen there is a need for specific version of SEMS, like those LTS versions project provides.
For that to work run the following command being in the directory of cloned repository:
```
git checkout --track origin/<version>
```

Example:
```
git checkout --track origin/mr13.2.1
```

_Tips and tricks: it may happen you have an older version of repository locally, and hence you lack certain newer versions, to fix that just run `git fetch origin`._

Compilation. The compilation process has some of basic package requirements:
```
g++, make, pkg-config
```

Alternatively you can just install `build-essential` package, which will bring you `g++`, `make` and other important tools and libraries.

There are also some specific dependencies, which some of the modules (applications) require:
```
libev-dev, libevent-dev, libhiredis-dev, libmysql++-dev, libopus-dev, libre2-dev, libspandsp-dev, libspeex-dev, libssl-dev, libxml2-dev, python3-dev
```

To start the compilation just run the following command from your local copy of repository,
this will compile the whole amount of available applications for you (.so objects) as well as the main executable (binary file):
```
make all
```

Now an executable binary file is placed in `core/` as well as all applications and modules compiled accordingly in `.so` format
and placed each in own directory (and also you can find them in `core/lib/`).

In order to deploy (install) them on the system, just run:
```
make install
```

_Installation will likely require sudo rights on the system._

This now makes SEMS ready to use on your installation. To proceed with configurations, see relevant part of the guide.

To clean a directory (local copy of SEMS repo) from compilation results, just run this:
```
make clean
```

## Compile for test and development purposes

SEMS project also supports a compilation for development and test purposes, like e.g. one would like to add certain fix via pull request
and quickly compile most important parts (after fix is added locally) execluding from compilation all extra stuff.

For that to work, run:
```
make TEST_ENVIRONMENT=yes
```

Same will apply for a case of cleaning:
```
make clean TEST_ENVIRONMENT=yes
```

_Tips and tricks for developers: use `-j` option with `make`, in order to compile using multi-threading (what makes it faster). Be cautious! Your system must have enough CPU resource to not get stuck._

## Compile specific parts

Compile SEMS core only:
```
make -C core
```

Compile specific module/application only:
```
make -C apps/dsm
```

Compile SEMS core and specific module/application:
```
make -C core && make -C apps/dsm
```
