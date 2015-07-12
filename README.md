### Install
``` bash
cd $GENODE_DIR/repos
git clone git://github.com/ehmry/genode-nix.git nix

for build_conf in $GENODE_DIR/build/*/etc/build.conf
do echo 'REPOSITORIES += $(GENODE_DIR)/repos/nix' >> $build_conf
done
```
