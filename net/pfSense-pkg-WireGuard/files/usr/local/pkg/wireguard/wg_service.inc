<?php
/*
 * wg_service.inc
 *
 * part of pfSense (https://www.pfsense.org)
 * Copyright (c) 2021 R. Christian McDonald (https://github.com/theonemcdonald)
 * All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


// pfSense includes
require_once('config.inc');
require_once('globals.inc');
require_once('gwlb.inc');
require_once('util.inc');
require_once('services.inc');
require_once('service-utils.inc');

// WireGuard includes
require_once('wireguard/wg.inc');

if (isset($argv[1])) {

	if (!wg_is_cli()) {

		// Bail out if we aren't in the CLI...
		die("FATAL: This script can only be started through the CLI.\n");

	}

	if (PHP_BINARY != $wgg['wg_daemon']) {

		// Bail out if we aren't running under under the correct executable...
		die("FATAL: This script can only be executed by {$wgg['wg_daemon']}.\n");

	}

	switch (strtolower($argv[1])) {

		case 'start':

			ignore_user_abort(true);

			set_time_limit(0);
	
			$ret_code = wg_service_start();
	
			exit($ret_code);

		case 'stop':

			ignore_user_abort(true);

			set_time_limit(0);
	
			wg_service_stop();
	
			exit(0);

		default:

			exit(1);

	}

}

function wg_is_cli() {

	return (PHP_SAPI == 'cli');

}

function wg_service_stop() {
	global $wgg;

	killbypid($wgg['pid_path']);
	
}

function wg_is_service_running() {
	global $wgg;

	if (!($h_lock = @fopen($wgg['pid_path'], 'a+')) || !file_exists($wgg['pid_path'])) {

		return false;

	}

	$not_running = flock($h_lock, LOCK_EX | LOCK_NB, $wouldblock);

	if ($not_running) {
	
		flock($h_lock, LOCK_UN);

	}

	fclose($h_lock);

	return (!$not_running || $wouldblock);

}

function wg_service_start() {
	global $g, $wgg;

	if (wg_is_service_running()) {

		return 1;

	}

	// Register the service environment and lock early to ensure singletons
	wg_register_service_env(false);

	// The initial fork...
	$newpid = pcntl_fork();

	if ($newpid === -1) {

		// We could not fork, so bail out...
		return 1;

	} elseif ($newpid) {

		// I'm the parent, and I'm going to self-destruct...
		return 0;

	}

	// Now become the session leader
	if (posix_setsid() < 0) {

		return 1;

	}

	// The second fork...
	$newpid = pcntl_fork();

	if ($newpid === -1) {

		// We could not fork, so bail out...
		return 1;

	} elseif ($newpid) {

		// I'm the parent, and I'm going to self-destruct...
		return 0;

	}

	// Now that we are properly daemonized, register the service signal handlers
	wg_register_sig_handler();

	if (platform_booting()) {

		$g['wireguard_firststart'] = true;

		print_message("Configuring WireGuard tunnels...");

		// Supresses ifconfig spew 
		mute_kernel_msgs();

	}
	
	// Build the tunnels
	wg_resync_all();
 
	if (platform_booting()) {

		unmute_kernel_msgs();

		print_message("done. \n");

		// Let the boot process continue and handle additional service and package invocation
		return 0;

	}

	// We don't need to restart any additional services because we just finished booting...
	if ($g['wireguard_firststart']) {

		unset($g['wireguard_firststart']);

		return 0;

	}

	// Spawns a child process to handle additional service restarting
	$newpid = pcntl_fork();

	if ($newpid === -1) {

		// We could not fork, so bail out...
		return 1;

	} elseif ($newpid) {

		// Wait for the child to finish
		pcntl_waitpid($newpid, $status);

		// We are the parent so go on...
		wg_service_daemon();

	// This is where we can now safely restart services
	} else {

		setup_gateways_monitor();

		services_unbound_configure();
		
		filter_configure();

		// All good
		return 0;

	}

	// We should never get here
	return 0;

}

function wg_service_daemon() {
	global $wgg;

	// Re-register the service environment now that we have finished restarting additional services
	wg_register_service_env(true);

	// Attempt to load the kmod, required to run the service without any tunnels configured
	if (!is_module_loaded($wgg['kmod'])) {

		$kmod = $wgg['kmod'];

		wg_escapeshellarg($kmod);

		mwexec("{$wgg['kldload']} {$kmod}");

	}

	// Main daemon loop
	while (true) {

		// Check if any new signals are waiting for dispatch
		pcntl_signal_dispatch();

		// The whole point of this daemon...
		if (!is_module_loaded($wgg['kmod'])) {

			break;

		}

		// Wait a bit before trying again
		sleep(1);

	}

	// Send a signal to self to terminate gracefully
	posix_kill(getmypid(), SIGTERM);

	// Fall though just in case
	return 0;

}

function wg_service_install() {
	global $wgg;

	wg_service_deinstall();

	link($wgg['php'], $wgg['wg_daemon']);

}

function wg_service_deinstall() {
	global $wgg;

	stop_service('wireguard');

	unlink_if_exists($wgg['wg_daemon']);

}

function wg_service_rcfile() {
	global $wgg;

	$start = $stop = array();

	$start[] = "{$wgg['wg_daemon']} -f {$wgg['wg_includes']}/wg_service.inc start";

	$stop[] = "{$wgg['wg_daemon']} -f {$wgg['wg_includes']}/wg_service.inc stop";

	$rcfile['file'] 	= 'wireguardd';

	$rcfile['start'] 	= implode("\n\t", $start);

	$rcfile['stop'] 	= implode("\n\t", $stop);

	write_rcfile($rcfile);

}

function wg_deregister_service_env() {
	global $h_lock, $wgg;

	// Release exclusive lock
	flock($h_lock, LOCK_UN);

	// Close file handler
	fclose($h_lock);

	// Delete PID
	unlink_if_exists($wgg['pid_path']);

}

function wg_register_service_env($close_handlers = false) {
	global $h_lock, $wgg;

	wg_deregister_service_env();

	$h_lock = fopen($wgg['pid_path'], 'a+');

	flock($h_lock, LOCK_EX);

	ftruncate($h_lock, 0);

	fseek($h_lock, 0, 0);

	fwrite($h_lock, getmypid());

	fflush($h_lock);

	if ($close_handlers) {

		fclose(STDIN);

		fclose(STDOUT);

		fclose(STDERR);

	}

}

function wg_register_sig_handler() {

	pcntl_signal(SIGTERM, 'sig_handler');

	pcntl_signal(SIGHUP, 'sig_handler');

	pcntl_signal(SIGINT, 'sig_handler');

}

function sig_handler($signo) {

	switch ($signo) {

		case SIGTERM:

			// Cleanup the service environment
			wg_deregister_service_env();

			// Teardown tunnels
			wg_destroy_tunnels();

			// We are done...
			exit(0);

			break;

		case SIGHUP:

			break;

		default:

			break;

	}

}

// This is a hack to get text onto the console during booting
function print_message($message) {

	$message = gettext($message);

    	$message = escapeshellarg($message);

	mwexec("printf {$message} >&2");

}

?>