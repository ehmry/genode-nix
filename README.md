This was an attempt to port the [Nix](https://nixos.org/nix/) package manager to 
[Genode](https://genode.org/) as prelude to bootstrapping Nixpkgs natively on 
Genode.

The current plan is solve cross-compilation of Nixpkgs from Linux to Genode, and 
then boostrap the Nix tools afterwards. See https://git.sr.ht/~ehmry/genodepkgs.
