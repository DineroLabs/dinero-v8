# Parent-first mining package selection

The pool selector previously retained the order in which its ancestor walk
discovered transactions. That order does not guarantee a parent precedes its
child when a high-fee transaction has multiple generations of ancestors.

After discovery, the actual selector now orders each complete package by its
dependencies. Multiple inputs from one parent create one dependency. Among
ready transactions, the smaller transaction ID goes first. A missing member,
identity mismatch or incomplete ordering refuses the package before selection
adds any transactions or resource counts. The existing exclusion, validity,
fee ranking, size, weight and proof resource checks remain in force.

Signed ordinary three-generation and diamond packages exercise the real
canonical ingress, pool capture and both actual assembler modes and job paths.
The tests also check shared-parent inputs, weight bounds, exclusions and
insertion-order stability. This does not enable Orchard admission or typed
Orchard block assembly, certify arbitrary pool resource arithmetic, or establish
release readiness. Mainnet activation remains unset.
