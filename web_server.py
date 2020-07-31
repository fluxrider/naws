# python -B web_server.py root_folder
import socket

# listen for clients
with socket.socket() as server:
  server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
  server.bind(('', 8888)) #TODO arg # NOTE: for privileged ports, sudo setcap 'cap_net_bind_service=+ep' /path/to/program
  server.listen(0)
  while True:
    client, client_address = server.accept()
    with client:

      # allow only the usual private IPv4 addresses
      print(f'client_address: {client_address}')
      allowed_ip = False
      allowed_ip |= client_address[0].startswith('192.168.')
      allowed_ip |= client_address[0] == ('127.0.0.1')
      if not allowed_ip: break
      # TODO if traffic from the internet/tor, turn on HTTPS/AUTH and turn server off on multi failed attempts

      # read client request
      data = client.recv(4096)
      print(data.decode())
# EXAMPLE
"""
GET / HTTP/1.1
Host: localhost:8888
User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:78.0) Gecko/20100101 Firefox/78.0
Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8
Accept-Language: en-US,en;q=0.5
Accept-Encoding: gzip, deflate
Connection: keep-alive
Upgrade-Insecure-Requests: 1
Cache-Control: max-age=0
"""

      http_response = b"""\
HTTP/1.1 200 OK

Hello, World!
"""
      client.sendall(http_response)
