# UNDER CONSTRUCTION

# Not Another Web Server
The server with no delusions of grandeur.

> Sometime all you need is a file.
> - David Lareau, when asked about why he hasn't worked with databases in 15+ years.

# Features

* Serves static files (e.g. web page, image, e-book).
* Serves output of executables or python scripts.

# Limitations
* Partial implementation of HTTP GET (and nothing else).
* IPv4 (and nothing else).
* Handles a single connection at a time.
* Accepts (only) connections from the home network (I force it due to lack of HTTPS).

# Roadmap

* HTTPS (some), so I can use it outside my home network.
* HTTP2 push
* Concurrent connections (but nothing that scales, lets say 10).