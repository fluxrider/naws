<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8" />
<title>401 Unauthorized</title>
<!-- NOTE: any scripts that this page needs is hardcoded in a no-auth list on the server -->
<script src="ricmoo.scrypt.with_libs.js"></script><!-- LICENSE https://github.com/ricmoo/scrypt-js -->
<script src="sodium.js"></script><!-- browser-sumo version, LICENSE https://github.com/jedisct1/libsodium.js -->
<script>
function gen_cookie() { 
  const button = document.getElementById("my_button");
  const username = document.getElementById("username").value.normalize('NFKC');
  const passphrase = document.getElementById("passphrase").value.normalize('NFKC');
  // passphrase + username + app_name to weaken same passphrase detection
  const password = sodium.from_string(passphrase + username + "demo_sanity_test");

  // private_key = key_derivation_function(password)
  function update_interface(progress) { button.value = "key derivation: " + progress; }
  const salt = sodium.from_string("static_salt".normalize('NFKC'));
  const N = 131072, r = 8, p = 1;
  const keyPromise = scrypt.scrypt(password, salt, N, r, p, 32, update_interface);
  keyPromise.then(function(private_key) {
    // NOTE: it is assumed a derived public key has been registed on the server for this username through other means
    document.getElementById("public_key").value = sodium.to_base64(sodium.crypto_scalarmult_base(private_key), sodium.base64_variants.ORIGINAL);
    // e.g. server can: echo -n 'base64string' | base64 -d > username.key
    
    // cookie = username + encrypt(server_timestamp)
    button.value = "encrypt server message";
    const nonce = sodium.randombytes_buf(sodium.crypto_box_NONCEBYTES);
    const server_public_key = SRV_PUB;
    const message = SRV_MSG;
    const encrypted = sodium.crypto_box_easy(message, nonce, server_public_key, private_key);
    button.value = "set cookies";
    set_cookie("nasm_username", sodium.to_base64(sodium.from_string(username), sodium.base64_variants.ORIGINAL), 2);
    set_cookie("nasm_proof", sodium.to_base64(encrypted, sodium.base64_variants.ORIGINAL), 2);
    set_cookie("nasm_proof_nonce", sodium.to_base64(nonce, sodium.base64_variants.ORIGINAL), 2);
    button.value = "done";
  });
}

function set_cookie(cname, cvalue, exdays) {
  var d = new Date(); d.setTime(d.getTime() + (exdays * 24 * 60 * 60 * 1000));
  document.cookie = cname + "=" + cvalue + ";expires=" + d.toUTCString() + ";path=/";
  //alert(cname + "=" + cvalue + ";expires=" + d.toUTCString() + ";path=/");
}
</script>
</head><body>
401 Unauthorized<br/>
<label for="username">Username:</label>
<input type="text" id="username" placeholder="e.g. john"/><br/><br/>
<label for="passphrase">Passphrase:</label>
<input type="password" id="passphrase" placeholder="e.g. elephant in the room" style="width: 50em;"/><br/><br/>
<input type="button" id="my_button" value="Generate signed cookie" onclick="gen_cookie();"/><br/><br/>
<a href="">Try again</a><br/><br/>
<input type="text" id="public_key" placeholder="i.e. your public key that should be registered with the server will appear here" style="width: 50em;"/><br/><br/>
</body>
</html>
