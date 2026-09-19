<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# The CA bundle Pkg carries on AROS

AROS has no system store of certificate authorities, so the AROS builds of
Pkg carry one inside the program. `cacert.pem` is that bundle, pinned here
and turned into a C array by `tools/build-aros.sh` and
`tools/build-aros-x86_64.sh`; the array itself is not checked in.

| | |
| --- | --- |
| File | `cacert.pem` |
| Source | <https://curl.se/ca/cacert.pem> |
| Certificate data from Mozilla as of | Thu Aug 13 03:12:01 2026 GMT |
| Certificates | 121 |
| SHA-256 | `f66dff1bdf8f96060b8177976f8b7d9254bc89bc4db933d769f7384d28480bc9` |

`PKG_CAFILE=<file>` names another bundle instead, for a machine behind a
private authority and for the tests, which make their own.

To renew it: download the file again, check that it parses
(`openssl crl2pkcs7 -nocrl -certfile cacert.pem | openssl pkcs7 -print_certs
-noout | head`), replace the three values in the table above and rebuild.

The certificates are the Mozilla root programme's, under the Mozilla Public
License 2.0; the bundle is assembled by the curl project.
