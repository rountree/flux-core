#!/bin/sh

test_description='Test flux job info service'

. `dirname $0`/kvs/kvs-helper.sh

. $(dirname $0)/sharness.sh

test_under_flux 4 job

wait_lookups_nonzero() {
        i=0
        while (! flux module stats --parse lookups job-info > /dev/null 2>&1 \
               || [ "$(flux module stats --parse lookups job-info 2> /dev/null)" = "0" ]) \
              && [ $i -lt 50 ]
        do
                sleep 0.1
                i=$((i + 1))
        done
        if [ "$i" -eq "50" ]
        then
            return 1
        fi
        return 0
}

test_expect_success 'job-info: generate jobspec for simple test job' '
        flux jobspec --format json srun -N1 hostname > test.json
'

#
# job eventlog tests
#

test_expect_success 'flux job eventlog works (active)' '
        jobid=$(flux job submit test.json) &&
	flux job eventlog $jobid > eventlog_a.out &&
        grep submit eventlog_a.out
'

test_expect_success 'flux job eventlog works on multiple entries (active)' '
        jobid=$(flux job submit test.json) &&
        kvsdir=$(flux job id --to=kvs-active $jobid) &&
	flux kvs eventlog append ${kvsdir}.eventlog foo &&
	flux job eventlog $jobid >eventlog_b.out &&
	grep -q submit eventlog_b.out &&
	grep -q foo eventlog_b.out
'

# we cheat and manually move active to inactive in these tests

test_expect_success 'flux job eventlog works (inactive)' '
        jobid=$(flux job submit test.json) &&
        activekvsdir=$(flux job id --to=kvs-active $jobid) &&
        inactivekvsdir=$(echo $activekvsdir | sed 's/active/inactive/') &&
        flux kvs move ${activekvsdir}.eventlog ${inactivekvsdir}.eventlog &&
	flux job eventlog $jobid > eventlog_c.out &&
        grep submit eventlog_c.out
'

test_expect_success 'flux job eventlog works on multiple entries (inactive)' '
        jobid=$(flux job submit test.json) &&
        activekvsdir=$(flux job id --to=kvs-active $jobid) &&
	flux kvs eventlog append ${activekvsdir}.eventlog foo &&
        inactivekvsdir=$(echo $activekvsdir | sed 's/active/inactive/') &&
        flux kvs move ${activekvsdir}.eventlog ${inactivekvsdir}.eventlog &&
	flux job eventlog $jobid >eventlog_d.out &&
	grep -q submit eventlog_d.out &&
	grep -q foo eventlog_d.out
'

test_expect_success 'flux job eventlog works on multiple entries (active -> inactive)' '
        jobid=$(flux job submit test.json) &&
        activekvsdir=$(flux job id --to=kvs-active $jobid) &&
        inactivekvsdir=$(echo $activekvsdir | sed 's/active/inactive/') &&
        flux kvs move ${activekvsdir}.eventlog ${inactivekvsdir}.eventlog &&
	flux kvs eventlog append ${inactivekvsdir}.eventlog foo &&
	flux job eventlog $jobid >eventlog_e.out &&
	grep -q submit eventlog_e.out &&
	grep -q foo eventlog_e.out
'

test_expect_success 'flux job eventlog fails on bad id' '
	! flux job eventlog 12345
'

test_expect_success 'flux job eventlog --context-format=json works' '
        jobid=$(flux job submit test.json) &&
	flux job eventlog --context-format=json $jobid > eventlog_format1.out &&
        grep -q "\"userid\":$(id -u)" eventlog_format1.out
'

test_expect_success 'flux job eventlog --context-format=text works' '
        jobid=$(flux job submit test.json) &&
	flux job eventlog --context-format=text $jobid > eventlog_format2.out &&
        grep -q "userid=$(id -u)" eventlog_format2.out
'

test_expect_success 'flux job eventlog --context-format=invalid fails' '
        jobid=$(flux job submit test.json) &&
	! flux job eventlog --context-format=invalid $jobid
'

#
# job wait-event tests
#

test_expect_success 'flux job wait-event works (active)' '
        jobid=$(flux job submit test.json) &&
        flux job wait-event $jobid submit > wait_event1.out &&
        grep submit wait_event1.out
'

test_expect_success 'flux job wait-event works (inactive)' '
        jobid=$(flux job submit test.json) &&
        activekvsdir=$(flux job id --to=kvs-active $jobid) &&
        inactivekvsdir=$(echo $activekvsdir | sed 's/active/inactive/') &&
        flux kvs move ${activekvsdir}.eventlog ${inactivekvsdir}.eventlog &&
        flux kvs eventlog append ${inactivekvsdir}.eventlog foobar &&
        flux job wait-event $jobid submit > wait_event2.out &&
        grep submit wait_event2.out
'

test_expect_success NO_CHAIN_LINT 'flux job wait-event works, event is later (active)' '
        jobid=$(flux job submit test.json)
        flux job wait-event $jobid foobar > wait_event3.out &
        waitpid=$! &&
        wait_lookups_nonzero &&
        wait_watcherscount_nonzero primary &&
        kvsdir=$(flux job id --to=kvs-active $jobid) &&
	flux kvs eventlog append ${kvsdir}.eventlog foobar &&
        wait $waitpid &&
        grep foobar wait_event3.out
'

# must carefully fake active -> inactive transition to avoid race
# where job-info module sees inactive transition but does not see any
# new events.  We do this by copying the eventlog, modifying the
# inactive eventlog, then removing the active one.

test_expect_success NO_CHAIN_LINT 'flux job wait-event works, event is later (active -> inactive) ' '
        jobid=$(flux job submit test.json)
        flux job wait-event $jobid foobar > wait_event4.out &
        waitpid=$! &&
        wait_lookups_nonzero &&
        wait_watcherscount_nonzero primary &&
        activekvsdir=$(flux job id --to=kvs-active $jobid) &&
        flux kvs eventlog append ${activekvsdir}.eventlog foobaz &&
        inactivekvsdir=$(echo $activekvsdir | sed 's/active/inactive/') &&
        flux kvs copy ${activekvsdir}.eventlog ${inactivekvsdir}.eventlog &&
        flux kvs eventlog append ${inactivekvsdir}.eventlog foobar &&
        flux kvs unlink ${activekvsdir}.eventlog &&
        wait $waitpid &&
        grep foobar wait_event4.out
'

test_expect_success 'flux job wait-event exits if never receives event (inactive) ' '
        jobid=$(flux job submit test.json) &&
        activekvsdir=$(flux job id --to=kvs-active $jobid) &&
        inactivekvsdir=$(echo $activekvsdir | sed 's/active/inactive/') &&
        flux kvs move ${activekvsdir}.eventlog ${inactivekvsdir}.eventlog &&
        ! flux job wait-event $jobid foobar > wait_event5.out 2> wait_event5.err &&
        ! test -s wait_event5.out &&
        grep "never received" wait_event5.err
'

test_expect_success NO_CHAIN_LINT 'flux job wait-event exits if never receives event (active -> inactive) ' '
        jobid=$(flux job submit test.json)
        flux job wait-event $jobid foobar > wait_event6.out 2> wait_event6.err &
        waitpid=$! &&
        wait_lookups_nonzero &&
        wait_watcherscount_nonzero primary &&
        activekvsdir=$(flux job id --to=kvs-active $jobid) &&
        inactivekvsdir=$(echo $activekvsdir | sed 's/active/inactive/') &&
        flux kvs move ${activekvsdir}.eventlog ${inactivekvsdir}.eventlog &&
        ! wait $waitpid &&
        ! test -s wait_event6.out &&
        grep "never received" wait_event6.err
'

test_expect_success 'flux job wait-event fails on bad id' '
	! flux job wait-event 12345 foobar
'

test_expect_success 'flux job wait-event --quiet works' '
        jobid=$(flux job submit test.json) &&
        flux job wait-event --quiet $jobid submit > wait_event7.out &&
        ! test -s wait_event7.out
'

test_expect_success 'flux job wait-event --verbose works' '
        jobid=$(flux job submit test.json) &&
        kvsdir=$(flux job id --to=kvs-active $jobid) &&
	flux kvs eventlog append ${kvsdir}.eventlog foobaz &&
	flux kvs eventlog append ${kvsdir}.eventlog foobar &&
        flux job wait-event --verbose $jobid foobar > wait_event8.out &&
        grep submit wait_event8.out &&
        grep foobaz wait_event8.out &&
        grep foobar wait_event8.out
'

test_expect_success 'flux job wait-event --verbose doesnt show events after wait event' '
        jobid=$(flux job submit test.json) &&
        kvsdir=$(flux job id --to=kvs-active $jobid) &&
	flux kvs eventlog append ${kvsdir}.eventlog foobar &&
        flux job wait-event --verbose $jobid submit > wait_event9.out &&
        grep submit wait_event9.out &&
        ! grep foobar wait_event9.out
'

test_expect_success 'flux job wait-event --timeout works' '
        jobid=$(flux job submit test.json) &&
        ! flux job wait-event --timeout=0.2 $jobid foobar 2> wait_event8.err &&
        grep "wait-event timeout" wait_event8.err
'

test_expect_success 'flux job wait-event hangs on no event' '
        jobid=$(flux job submit test.json) &&
        ! run_timeout 0.2 flux job wait-event $jobid foobar
'

test_expect_success 'job-info stats works' '
        flux module stats job-info | grep "lookups"
'

test_done