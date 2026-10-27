-- packaging/sql/log_silence_index.sql
CREATE INDEX IF NOT EXISTS ix_resource_monitor_eqp_recv_count
ON CloudESM.SUM_COLLECT_INFO (eqp_ip, recv_time, `count`)
ALGORITHM=INPLACE LOCK=NONE;

SHOW INDEX FROM CloudESM.SUM_COLLECT_INFO;
