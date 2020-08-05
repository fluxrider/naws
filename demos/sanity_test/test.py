import cgi
import os
import sys
import time

qs = cgi.FieldStorage()
if 'user' in qs and qs['user'].value.isalpha():
  user = qs['user'].value
else:
  user = 'anonymous'

print(f"""Content-Type:text/html;charset=utf-8

<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8" />
<title>python script</title>
</head><body>
python script for {user} at {os.getcwd()}.
</body>
</html>
""")

# force a 404
# sys.exit(4)

# get stuck
# time.sleep(10)
