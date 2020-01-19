--TEST--
Check if photon is loaded
--SKIPIF--
<?php
if (!extension_loaded('photon')) {
	echo 'skip';
}
?>
--FILE--
<?php
echo 'The extension "photon" is available';
?>
--EXPECT--
The extension "photon" is available
