#!/bin/sh

test_description='Test flux job exec configuration'

. $(dirname $0)/sharness.sh

test_under_flux 1 job

test_expect_success 'job-exec: can specify kill-timeout on cmdline' '
	flux dmesg -C &&
	flux module reload job-exec kill-timeout=1m &&
	flux dmesg | grep "using kill-timeout of 60s"
'
test_expect_success 'job-exec: bad kill-timeout value causes module failure' '
	flux dmesg -C &&
	test_expect_code 1 flux module reload job-exec kill-timeout=1f &&
	flux dmesg | grep "invalid kill-timeout: 1f"
'
test_expect_success 'job-exec: kill-timeout can be set in exec conf' '
	name=killconf &&
	cat <<-EOF > ${name}.toml &&
	[exec]
	kill-timeout = ".5m"
	EOF
	flux start -o,--config-path=${name}.toml -s1 \
		flux dmesg > ${name}.log 2>&1 &&
	grep "using kill-timeout of 30s" ${name}.log
'
test_expect_success 'job-exec: bad kill-timeout config causes module failure' '
	name=bad-killconf &&
	cat <<-EOF > ${name}.toml &&
	[exec]
	kill-timeout = "foo"
	EOF
	test_must_fail flux start -o,--config-path=${name}.toml -s1 \
		flux dmesg > ${name}.log 2>&1 &&
	grep "invalid kill-timeout: foo" ${name}.log
'
test_expect_success 'job-exec: can specify default-shell on cmdline' '
	flux dmesg -C &&
	flux module reload -f job-exec job-shell=/path/to/shell &&
	flux dmesg &&
	flux dmesg | grep "default shell path /path/to/shell"
'
test_expect_success 'job-exec: job-shell can be set in exec conf' '
	name=shellconf &&
	cat <<-EOF > ${name}.toml &&
	[exec]
	job-shell = "my-flux-shell"
	EOF
	flux start -o,--config-path=${name}.toml -s1 \
		flux dmesg > ${name}.log 2>&1 &&
	grep "using default shell path my-flux-shell" ${name}.log
'
test_expect_success 'job-exec: bad job-shell config causes module failure' '
	name=bad-shellconf &&
	cat <<-EOF > ${name}.toml &&
	[exec]
	job-shell = 42
	EOF
	test_must_fail flux start -o,--config-path=${name}.toml -s1 \
		flux dmesg > ${name}.log 2>&1 &&
	grep "error reading config value exec.job-shell" ${name}.log
'
test_expect_success 'job-exec: can specify imp path on cmdline' '
	flux dmesg -C &&
	flux module reload -f job-exec imp=/path/to/imp &&
	flux dmesg &&
	flux dmesg | grep "using imp path /path/to/imp"
'
test_expect_success 'job-exec: imp path can be set in exec conf' '
	name=impconf &&
	cat <<-EOF > ${name}.toml &&
	[exec]
	imp = "my-flux-imp"
	EOF
	flux start -o,--config-path=${name}.toml -s1 \
		flux dmesg > ${name}.log 2>&1 &&
	grep "using imp path my-flux-imp" ${name}.log
'
test_expect_success 'job-exec: bad imp config causes module failure' '
	name=bad-impconf &&
	cat <<-EOF > ${name}.toml &&
	[exec]
	imp = 42
	EOF
	test_must_fail flux start -o,--config-path=${name}.toml -s1 \
		flux dmesg > ${name}.log 2>&1 &&
	grep "error reading config value exec.imp" ${name}.log
'

test_done
