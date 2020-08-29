# UNDER CONSTRUCTION

# Not Another Web Server

The server with no delusions of grandeur.

Raison D'être: Personal web sites meant for a single user and their immediate family have simple needs. High-performance web servers become awkward to use for such simple cases (e.g. nginx lacks CGI support). Flexible web servers let you shoot yourself in the foot (e.g. picking outdated HTTPS settings, lighttpd uses static-file.exclude-extensions instead of the other way around).

> Sometime all you need is a file.
> - David Lareau, when asked about why he hasn't worked with databases in 15+ years.

# Features

* Accepts (only) connections from the home network (unless coming through tor).
* Serves the usual static files (e.g. web page, image, e-book).
* Serves output of arbitrary executables and python scripts.
	* Programs that spew to standard error or have non-zero exit code will cause a HTTP 500.
	* Programs that return 4 will cause a HTTP 404.
	* Programs control (and are expected to set) the Content Type.
	* Hash Bang executables do not need execute permission to run (thus can be stored on non-posix filesystem).
* Built-in cookie-based public-key-based access authentication (for traffic coming through tor).

# Limitations

* Allowing scripts better control over HTTP 500 and 404 comes at a memory and speed price. The output is buffered (without limit) until it exits and then sent to the client. Each thread has separate buffers.
* Partial implementation of HTTP GET (and nothing else).
* IPv4 (and nothing else).

# Backburner (a.k.a. won't do [probably])

* HTTPS / TLS
	* commercial VPN don't usually allow NAT port forwarding, making having a public facing clearnet web site moot.
		* I find setting up a tor hidden service simpler and it comes with it's own encryption.
			* Having a signed certificate approved by a CA for HTTPS for an onion address is asking too much.
		* Part of my design is to prevent unauthorized access even if a firewall lets the traffic through. Not using a VPN exposes the IP, and an attacker, even if unsuccessful can slow down your network, and that is true even if you shutdown the web server. A VPN provider absorbs this cost (unless it allows port forwarding which I can't recommend).
* HTTP2 push
	* For misguided reasons, all major browsers only support HTTP/2 over TLS, so to implement push, I'd need to also implement HTTPS support.
	* I'm not sure how much of HTTP2 I would have to support until I can do push, and the whole thing looks pretty complicated to implement.
