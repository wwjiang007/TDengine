from new_test_framework.utils import tdLog, tdSql, sc, clusterComCheck


class TestFuncCount:

    def setup_class(cls):
        tdLog.debug(f"start to execute {__file__}")

    def test_func_count(self):
        """Count 函数

        1. 创建包含一个 Int 普通数据列的超级表
        2. 创建子表并写入数据
        3. 对子表执行 Count 查询，包括时间窗口、普通数据列筛选
        4. 对超级表执行 Count 查询，包括时间窗口、普通数据列筛选、标签列筛选、Group By、Partition By

        Catalog:
            - Function:Aggregate

        Since: v3.0.0.0

        Labels: common,ci

        Jira: None

        History:
            - 2025-4-28 Simon Guan Migrated from tsim/compute/count.sim

        """

        dbPrefix = "m_co_db"
        tbPrefix = "m_co_tb"
        mtPrefix = "m_co_mt"
        tbNum = 10
        rowNum = 20
        totalNum = 200

        tdLog.info(f"=============== step1")
        i = 0
        db = dbPrefix + str(i)
        mt = mtPrefix + str(i)

        tdSql.prepare(db, drop=True)
        tdSql.execute(f"use {db}")
        tdSql.execute(f"create table {mt} (ts timestamp, tbcol int) TAGS(tgcol int)")

        i = 0
        while i < tbNum:
            tb = tbPrefix + str(i)
            tdSql.execute(f"create table {tb} using {mt} tags( {i} )")
            x = 0
            while x < rowNum:
                cc = x * 60000
                ms = 1601481600000 + cc
                tdSql.execute(f"insert into {tb} values ({ms} , {x} )")
                x = x + 1
            i = i + 1

        tdLog.info(f"=============== step2")
        i = 1
        tb = tbPrefix + str(i)

        tdSql.query(f"select count(*) from {tb}")
        tdLog.info(f"===> select count(*) from {tb} => {tdSql.getData(0,0)}")
        tdSql.checkData(0, 0, rowNum)

        tdSql.query(f"select count(1) from {tb}")
        tdLog.info(f"===> select count(1) from {tb} => {tdSql.getData(0,0)}")
        tdSql.checkData(0, 0, rowNum)

        tdSql.query(f"select count(tbcol) from {tb}")
        tdLog.info(f"===> {tdSql.getData(0,0)}")
        tdSql.checkData(0, 0, rowNum)

        tdLog.info(f"=============== step3")
        cc = 4 * 60000
        ms = 1601481600000 + cc
        tdSql.query(f"select count(tbcol) from {tb} where ts <= {ms}")
        tdLog.info(f"===> {tdSql.getData(0,0)}")
        tdSql.checkData(0, 0, 5)

        tdLog.info(f"=============== step4")
        tdSql.query(f"select count(tbcol) as b from {tb}")
        tdLog.info(f"===> {tdSql.getData(0,0)}")
        tdSql.checkData(0, 0, rowNum)

        tdLog.info(f"=============== step5")
        tdSql.query(f"select count(tbcol) as b from {tb} interval(1m)")
        tdLog.info(f"===> {tdSql.getData(0,0)}")
        tdSql.checkData(0, 0, 1)

        tdSql.query(f"select count(tbcol) as b from {tb} interval(1d)")
        tdLog.info(f"===> {tdSql.getData(0,0)}")
        tdSql.checkData(0, 0, rowNum)

        tdLog.info(f"=============== step6")
        cc = 4 * 60000
        ms = 1601481600000 + cc
        tdSql.query(f"select count(tbcol) as b from {tb} where ts <= {ms} interval(1m)")
        tdLog.info(f"===> {tdSql.getData(0,0)}")
        tdSql.checkData(0, 0, 1)
        tdSql.checkRows(5)

        tdLog.info(f"=============== step7")
        tdSql.query(f"select count(*) from {mt}")
        tdLog.info(f"===> {tdSql.getData(0,0)}")
        tdSql.checkData(0, 0, totalNum)

        tdLog.info(f"=============== step8")
        tdSql.query(f"select count(1) from {mt}")
        tdLog.info(f"===> {tdSql.getData(0,0)}")
        tdSql.checkData(0, 0, totalNum)

        tdSql.query(f"select count(tbcol) from {mt}")
        tdLog.info(f"===> {tdSql.getData(0,0)}")
        tdSql.checkData(0, 0, totalNum)

        tdLog.info(f"=============== step10")
        cc = 4 * 60000
        ms = 1601481600000 + cc
        tdSql.query(f"select count(tbcol) as c from {mt} where ts <= {ms}")
        tdLog.info(f"===> {tdSql.getData(0,0)}")
        tdSql.checkData(0, 0, 50)

        tdSql.query(f"select count(tbcol) as c from {mt} where tgcol < 5")
        tdLog.info(f"===> {tdSql.getData(0,0)}")
        tdSql.checkData(0, 0, 100)

        cc = 4 * 60000
        ms = 1601481600000 + cc
        tdSql.query(
            f"select count(tbcol) as c from {mt} where tgcol < 5 and ts <= {ms}"
        )
        tdLog.info(f"===> {tdSql.getData(0,0)}")
        tdSql.checkData(0, 0, 25)

        tdLog.info(f"=============== step9")
        tdSql.query(f"select count(tbcol) as b from {mt} interval(1m)")
        tdLog.info(f"===> {tdSql.getData(0,0)}")
        tdSql.checkData(0, 0, 10)
        tdSql.checkData(1, 0, 10)

        tdSql.query(f"select count(tbcol) as b from {mt} interval(1d)")
        tdLog.info(f"===> {tdSql.getData(0,0)}")
        tdSql.checkData(0, 0, 200)

        tdLog.info(f"=============== step10")
        tdSql.query(f"select count(tbcol) as b from {mt} group by tgcol")
        tdLog.info(f"===> {tdSql.getData(0,0)}")
        tdSql.checkData(0, 0, rowNum)

        tdSql.checkRows(tbNum)

        tdLog.info(f"=============== step11")
        cc = 4 * 60000
        ms = 1601481600000 + cc
        tdSql.query(
            f"select count(tbcol) as b from {mt}  where ts <= {ms} partition by tgcol interval(1m)"
        )
        tdSql.checkData(0, 0, 1)
        tdSql.checkRows(50)

        tdLog.info(f"=============== clear")
        tdSql.execute(f"drop database {db}")
        tdSql.query(f"select * from information_schema.ins_databases")
        tdSql.checkRows(2)
