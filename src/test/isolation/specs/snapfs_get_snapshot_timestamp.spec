# The recent just created snapshot cannot be older than 1 minute :)

teardown {
	select pg_remove_snapshot( ( select recent_snapshot from pg_control_snapshot() ) );
	drop table foo;
}

session "s1"
step "s1_mk_sn" {
	select 1 as "make_snapshot" from pg_make_snapshot();
	create table foo as select 'test_data';
	select ( now() - ( select pg_get_snapshot_timestamp( ( select recent_snapshot from pg_control_snapshot() ) ) ) ) between '0 sec'::interval and '1 min'::interval as "timestamp_check"
}

permutation "s1_mk_sn"
