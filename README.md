### This project is dead.

The goal was to create a automagical package manager that could
make packages appear using nothing but special dynamic linker
routing and a virtual file-system layer. Both those things worked
and it was fun to watch, but the amount of hacking it took to
produce anything substantial passed the point of diminishing
returns.

I think still think Nix is the best package manager available
but it is not a good package manager for Genode. The expression
language is domain specific and that domain is Unix. Many of
the features of Nix are redundant with the features of Genode,
so to fully implement them was redundant, and worse, confusing.
Hiding Nix store paths at runtime is a cool feature but presents
numerous problems when evaulating both Genode and Unix-on-Genode
packages.

Evaulating Unix packages from the Nixpkgs collection to compile
for Genode targets was and still is a worthwhile line of work.
However, the performance of the Genode Unix runtime and the
file-system layer relative to native Unix environments necessarily
prioritizes itself over a robust and self-hosting Nixpkgs port.
