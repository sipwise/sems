# Packaging

This is an alternative to manual compilation of SEMS from sources.

SEMS project provides a possibility to build packages for further installation on systems (e.g. debian OS)
based on given sources in repository.
This guide gives short and simple explanation how to quickly prepare SEMS packages for usage.

## Clone

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

## Built packages with sbuild

Install sbuild developers environment and pristine:
```
apt install sbuild-debian-developer-setup pristine-tar
```

Configure and start sbuild environment (for that you might will need sudo permissions):
```
sbuild-debian-developer-setup --suite=bookworm
```

_In this example as a suite the bookworm distro is selected_

Previously run command should add your user into sbuild and will add some required directories.
After that is done, run the following command:
```
newgrp sbuild
```

Now the future package is to be built in a proper suite:
```
gbp buildpackage --git-builder=sbuild \
--git-debian-branch=master -d bookworm --no-clean-source 
```

_In this example we build master version of SEMS for the debian bookworm distribution._

## Missing dependencies

Sometimes one has to deal with missing dependencies:
```
The following packages have unmet dependencies:                                                          
 sbuild-build-depends-main-dummy : Depends: portaudio19-dev but it is not going to be installed          
                                   Depends: systemd-dev but it is not installable
E: Unable to correct problems, you have held broken packages.
apt-get failed.                                    
E: Package installation failed         
```

To fix that just install required packages (e.g. `portaudio19-dev`, `systemd-dev` or alike) from the official repository of your distribution.

## Results

Now in the ../ directory there should be a list of files created like:
```
ngcp-sems_1.6.0+0~mr13.3.0.0_amd64.deb
ngcp-sems-dbgsym_1.6.0+0~mr13.3.0.0_amd64.deb
ngcp-sems_1.6.0+0~mr13.3.0.0.tar.xz
ngcp-sems_1.6.0+0~mr13.3.0.0_amd64-2025-03-01T20:45:27Z.build
ngcp-sems_1.6.0+0~mr13.3.0.0_amd64.build -> ngcp-sems_1.6.0+0~mr13.3.0.0_amd64-2025-03-01T20:45:27Z.build
ngcp-sems_1.6.0+0~mr13.3.0.0_amd64.buildinfo
ngcp-sems_1.6.0+0~mr13.3.0.0_amd64.changes
ngcp-sems_1.6.0+0~mr13.3.0.0.dsc
```

From those only .deb packages will be required to deploy SEMS binaries on the wished system,
so the `ngcp-sems_<version>` and `ngcp-sems-dbgsym_<version>` one. Whereas dbgsym isn't really required
for a normal run of this binary, but is only needed for a future coredump investigation using gdb.
