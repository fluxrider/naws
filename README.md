# UNDER CONSTRUCTION

# Not Another Web Server

The server with no delusions of grandeur.

Raison D'être: Personal web sites meant for a single user and their immediate family have simple needs. High-performance web servers become awkward to use for such simple cases (e.g. nginx lacks CGI support). Flexible web servers let you shoot yourself in the foot (e.g. picking outdated HTTPS settings, lighttpd uses static-file.exclude-extensions instead of the other way around).

> Sometime all you need is a file.
> - David Lareau, when asked about why he hasn't worked with databases in 15+ years.

# Features

* Serves the usual static files (e.g. web page, image, e-book).
* Serves output of arbitrary executables and python scripts.
	* Programs that spew to standard error or have non-zero exit code will cause a HTTP 500.
	* Programs that return 4 will cause a HTTP 404.
	* Programs control (and are expected to set) the Content Type.
	* Hash Bang executables do not need execute permission to run.
* Built-in cookie-based access authentication (for traffic coming from tor).

# Limitations

* Partial implementation of HTTP GET (and nothing else).
* IPv4 (and nothing else).
* Handles a single connection at a time.
* Accepts (only) connections from the home network (unless coming through tor).

# Roadmap

* HTTPS (some), so it can accept connections from outside the home network from the clear web.
* HTTP2 push
* Concurrent connections (but nothing that scales, lets say 10, so I can open in new window a few things and benefit from the parallelism).