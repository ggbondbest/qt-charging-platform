"""Operational invariants on an actual, uniquely named MySQL test schema.

RUN_MYSQL_TESTS=1 MYSQL_TEST_HOST/PORT/USER/PASSWORD select an isolated server.
Only schemas created by this test instance are dropped. No SQLite or mocked DB.
"""

from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timedelta, timezone
import importlib.util
import json
import os
import time
import unittest
import uuid

from data_analysis.chargepilot.store import ChargePilotStore, DomainError
from data_analysis.mysql_support import MySQLSettings, connect


CATALOG = [dict(stationId="S1", cityId="C1", stationName="第一站", capacity=1,
                powerKw=60, pricePerKwh=2, latitude=31.2, longitude=121.4),
           dict(stationId="S2", cityId="C1", stationName="第二站", capacity=2,
                powerKw=120, pricePerKwh=1.5, latitude=31.3, longitude=121.5)]
CITIES = [dict(cityId="C1", cityName="模拟城", latitude=31.2, longitude=121.4)]


@unittest.skipUnless(os.environ.get("RUN_MYSQL_TESTS") == "1" and importlib.util.find_spec("pymysql"),
                     "Requires RUN_MYSQL_TESTS=1 and a task-isolated real MySQL server")
class ChargePilotStoreTests(unittest.TestCase):
    def setUp(self):
        self.schema = "cp_store_test_" + uuid.uuid4().hex[:20]
        self.settings = MySQLSettings(host=os.environ.get("MYSQL_TEST_HOST", "127.0.0.1"),
            port=int(os.environ.get("MYSQL_TEST_PORT", "3306")),
            user=os.environ.get("MYSQL_TEST_USER", "root"),
            password=os.environ.get("MYSQL_TEST_PASSWORD", ""), database=self.schema)
        connection = connect(self.settings, database="")
        try:
            with connection.cursor() as cursor:
                cursor.execute(f"CREATE DATABASE `{self.schema}` CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin")
            connection.commit()
        finally:
            connection.close()
        self.addCleanup(self.cleanup_schema)
        self.store = ChargePilotStore(self.settings)
        self.store.initialize(CATALOG, CITIES)
        self.store.control_clock({"action": "configure", "paused": True})
        self.store.configure({"rewards": {"first": 100, "second": 60}})

    def cleanup_schema(self):
        self.assertTrue(self.schema.startswith("cp_store_test_"))
        connection = connect(self.settings, database="")
        try:
            with connection.cursor() as cursor:
                cursor.execute(f"DROP DATABASE `{self.schema}`")
            connection.commit()
        finally:
            connection.close()

    def user(self, name="测试用户"):
        return self.store.create_session(name)["user"]["userId"]

    def offer(self, user, *, energy=20, eta=0, expiry=900, rid=None, multiplier=1, price=2):
        now = datetime.fromisoformat(self.store.clock()["time"].replace("Z", "+00:00"))
        return self.store.save_recommendation(user, dict(recommendationId=rid or str(uuid.uuid4()),
            origin={"latitude": 31.2, "longitude": 121.4}, energyKwh=energy,
            expiresAt=(now + timedelta(seconds=expiry)).isoformat(),
            candidates=[dict(stationId="S1", rank=1, score=.9, rewardPoints=999999,
                             etaMinutes=eta, rewardMultiplier=multiplier, pricePerKwh=price),
                        dict(stationId="S2", rank=2, score=.8, etaMinutes=eta)]))

    def select(self, user, station="S1", **kwargs):
        offer = self.offer(user, **kwargs)
        return self.store.select(user, offer["recommendationId"], station)

    def start(self, user, station="S1", **kwargs):
        trip = self.select(user, station, **kwargs)
        self.store.act(user, trip["tripId"], "arrive")
        return self.store.act(user, trip["tripId"], "start")

    def advance(self, seconds):
        return self.store.control_clock({"action": "advance", "seconds": seconds})

    def status(self, user):
        return self.store.me(user)["trips"][0]

    def assert_domain(self, code, fn, *args, status=None):
        with self.assertRaises(DomainError) as caught:
            fn(*args)
        self.assertEqual(caught.exception.code, code)
        if status is not None:
            self.assertEqual(caught.exception.status, status)

    def test_session_hash_only_and_ownership(self):
        session = self.store.create_session("  用户甲  ")
        owner, other = session["user"]["userId"], self.user("乙")
        self.assertEqual(self.store.authenticate(session["token"]), owner)
        self.assertEqual(session["user"]["name"], "用户甲")
        connection = connect(self.settings, dict_rows=True)
        try:
            with connection.cursor() as cursor:
                cursor.execute("SELECT * FROM cp_users WHERE user_id=%s", (owner,))
                row = cursor.fetchone()
                self.assertEqual(len(row["token_hash"]), 64)
                self.assertNotIn(session["token"], str(row))
        finally:
            connection.close()
        self.assert_domain("UNAUTHORIZED", self.store.authenticate, "not-a-token", status=401)
        trip = self.select(owner)
        self.assert_domain("TRIP_NOT_FOUND", self.store.act, other, trip["tripId"], "cancel", status=404)
        self.assert_domain("RECOMMENDATION_NOT_FOUND", self.store.select, other, trip["recommendationId"], "S1", status=404)

    def test_offer_immutable_reward_config_and_single_active_trip(self):
        user = self.user()
        offer = self.offer(user)
        self.assertEqual(offer["candidates"][0]["rewardPoints"], 100)
        self.store.configure({"rewards": {"first": 900, "second": 800}, "minimumEnergyKwh": 9})
        altered = dict(offer, energyKwh=400, origin={"latitude": 0, "longitude": 0},
                       candidates=[dict(stationId="S2", rank=1, etaMinutes=0)])
        self.assertEqual(self.store.save_recommendation(user, altered), offer)
        trip = self.store.select(user, offer["recommendationId"], "S1")
        self.assertEqual(trip["rewardPoints"], 100)
        self.assertEqual(trip["origin"], offer["origin"])
        self.assertEqual(self.store.select(user, offer["recommendationId"], "S1")["tripId"], trip["tripId"])
        self.assert_domain("OFFER_ALREADY_USED", self.store.select, user, offer["recommendationId"], "S2")
        newer = self.offer(user)
        self.assert_domain("ACTIVE_TRIP", self.store.select, user, newer["recommendationId"], "S2")
        self.store.act(user, trip["tripId"], "arrive")
        self.store.act(user, trip["tripId"], "start")
        self.advance(120)
        self.store.act(user, trip["tripId"], "stop")
        self.assert_domain("ACTIVE_TRIP", self.store.select, user, newer["recommendationId"], "S2")
        paid = self.store.act(user, trip["tripId"], "pay")
        self.assertEqual(paid["awardedPoints"], 100, "Offer retained its earlier energy threshold")

    def test_eta_expired_offer_and_invalid_selection(self):
        user = self.user()
        offer = self.offer(user, expiry=5)
        self.assert_domain("INELIGIBLE_STATION", self.store.select, user, offer["recommendationId"], "nonexistent")
        self.advance(6)
        self.assert_domain("RECOMMENDATION_EXPIRED", self.store.select, user, offer["recommendationId"], "S1")
        trip = self.select(user, eta=2)
        self.assert_domain("ARRIVAL_TOO_EARLY", self.store.act, user, trip["tripId"], "arrive")
        self.advance(121)
        self.assertEqual(self.store.act(user, trip["tripId"], "arrive")["status"], "RESERVED")

    def test_fifo_arrival_order_and_reservation_expiry_calls_next(self):
        self.store.configure({"callTimeoutSeconds": 30, "reservationTimeoutSeconds": 40})
        self.store.set_background("S1", 1, 60)
        earlier_selected, first_arrival, last = self.user("A"), self.user("B"), self.user("C")
        a, b, c = self.select(earlier_selected), self.select(first_arrival), self.select(last)
        for user, trip in ((first_arrival, b), (earlier_selected, a), (last, c)):
            self.store.act(user, trip["tripId"], "arrive")
        self.assertEqual(self.status(earlier_selected)["queuePosition"], 2)
        self.advance(60)
        self.assertEqual(self.status(first_arrival)["status"], "CALLED")
        self.assertEqual(self.store.stations()[0]["currentFree"], 0)
        self.store.act(first_arrival, b["tripId"], "confirm")
        self.advance(41)
        self.assertEqual(self.status(first_arrival)["status"], "EXPIRED")
        self.assertEqual(self.status(earlier_selected)["status"], "CALLED")
        self.assertEqual(self.status(last)["queuePosition"], 1)
        self.store.act(earlier_selected, a["tripId"], "cancel")
        self.assertEqual(self.status(last)["status"], "CALLED")

    def test_large_clock_jump_processes_calls_at_event_time(self):
        self.store.configure({"callTimeoutSeconds": 20})
        self.store.set_background("S1", 1, 10)
        users = [self.user(str(i)) for i in range(3)]
        for user in users:
            trip = self.select(user)
            self.store.act(user, trip["tripId"], "arrive")
        self.advance(55)
        self.assertEqual([self.status(user)["status"] for user in users], ["EXPIRED", "EXPIRED", "CALLED"])
        expires = datetime.fromisoformat(self.status(users[2])["callExpiresAt"].replace("Z", "+00:00"))
        now = datetime.fromisoformat(self.store.clock()["time"].replace("Z", "+00:00"))
        self.assertAlmostEqual((expires - now).total_seconds(), 15, delta=.005)
        self.advance(16)
        self.assertEqual(self.store.stations()[0]["currentFree"], 1)

    def test_stop_frees_port_before_payment_server_energy_and_points_once(self):
        owner, waiting = self.user(), self.user("排队")
        trip = self.start(owner)
        second = self.select(waiting)
        self.store.act(waiting, second["tripId"], "arrive")
        self.advance(120)
        stopped = self.store.act(owner, trip["tripId"], "stop")
        self.assertAlmostEqual(stopped["chargingSeconds"], 120, delta=.005)
        self.assertAlmostEqual(stopped["energyKwh"], 2, delta=.001)
        self.assertEqual(stopped["amount"], 4)
        self.assertEqual(self.status(waiting)["status"], "CALLED")
        self.assertEqual(self.store.me(owner)["user"]["points"], 0)
        with ThreadPoolExecutor(max_workers=4) as executor:
            payments = list(executor.map(lambda _: self.store.act(owner, trip["tripId"], "pay"), range(4)))
        self.assertEqual({paid["awardedPoints"] for paid in payments}, {100})
        self.assertEqual(self.store.me(owner)["user"]["points"], 100)
        self.assertEqual(len(self.store.me(owner)["ledger"]), 1)
        self.assertEqual(self.store.admin()["counts"]["payments"], 1)

    def test_automatic_target_stop_does_not_charge_until_poll(self):
        owner, waiting = self.user(), self.user("排队")
        trip = self.start(owner, energy=1)
        second = self.select(waiting)
        self.store.act(waiting, second["tripId"], "arrive")
        self.advance(80)
        stopped = self.status(owner)
        self.assertEqual(stopped["status"], "PENDING_PAYMENT")
        self.assertEqual(stopped["stopReason"], "TARGET_REACHED")
        self.assertEqual(stopped["energyKwh"], 1)
        self.assertAlmostEqual(stopped["chargingSeconds"], 60, delta=.005)
        self.assertEqual(stopped["amount"], 2)
        self.assertEqual(self.status(waiting)["status"], "CALLED")
        self.assertEqual(self.store.act(owner, trip["tripId"], "pay")["awardedPoints"], 100)

    def test_short_zero_cancelled_and_expired_trip_rewards(self):
        owner = self.user()
        trip = self.start(owner)
        self.store.act(owner, trip["tripId"], "stop")
        paid = self.store.act(owner, trip["tripId"], "pay")
        self.assertEqual(paid["awardedPoints"], 0)
        self.assertEqual(paid["rewardStatus"], "NO_POSITIVE_PAYMENT")
        trip = self.start(owner)
        self.advance(30)
        self.store.act(owner, trip["tripId"], "stop")
        self.assertEqual(self.store.act(owner, trip["tripId"], "pay")["rewardStatus"], "BELOW_MINIMUM_DURATION")
        trip = self.select(owner)
        self.store.act(owner, trip["tripId"], "cancel")
        self.assert_domain("INVALID_TRANSITION", self.store.act, owner, trip["tripId"], "pay")
        self.assertEqual(self.store.me(owner)["user"]["points"], 0)

    def test_payment_after_reward_expiry_never_awards(self):
        self.store.configure({"rewardEligibilitySeconds": 100})
        owner = self.user()
        trip = self.start(owner, energy=1)
        self.advance(101)
        paid = self.store.act(owner, trip["tripId"], "pay")
        self.assertEqual(paid["rewardStatus"], "REWARD_EXPIRED")
        self.assertEqual(paid["awardedPoints"], 0)

    def test_concurrent_capacity_claims_cannot_overbook(self):
        users = [self.user(str(i)) for i in range(6)]
        trips = [self.select(user, "S2") for user in users]
        with ThreadPoolExecutor(max_workers=6) as executor:
            results = list(executor.map(lambda pair: self.store.act(pair[0], pair[1]["tripId"], "arrive"), zip(users, trips)))
        self.assertEqual(sum(trip["status"] == "RESERVED" for trip in results), 2)
        station = self.store.stations()[1]
        self.assertEqual((station["currentFree"], station["committed"], station["queued"]), (0, 2, 4))

    def test_daily_budget_atomic_across_different_payments(self):
        self.store.configure({"dailyRewardBudget": 90})
        users = [self.user("A"), self.user("B")]
        trips = [self.start(user, "S2", energy=2) for user in users]
        self.advance(65)
        with ThreadPoolExecutor(max_workers=2) as executor:
            paid = list(executor.map(lambda pair: self.store.act(pair[0], pair[1]["tripId"], "pay"), zip(users, trips)))
        self.assertEqual(sum(trip["awardedPoints"] for trip in paid), 90)
        self.assertEqual(sorted(trip["awardedPoints"] for trip in paid), [30, 60])
        self.assertEqual(sum(self.store.me(user)["user"]["points"] for user in users), 90)

    def test_baseline_once_and_admin_cannot_displace_user(self):
        owner = self.user()
        trip = self.start(owner, "S2")
        self.assertFalse(self.store.admin()["baselineInitialized"])
        self.store.set_baseline([{"stationId": "S2", "currentFree": 0, "releaseAfterSeconds": 10}])
        self.assertTrue(self.store.admin()["baselineInitialized"])
        station = self.store.stations()[1]
        self.assertEqual((station["backgroundBusy"], station["charging"], station["currentFree"]), (1, 1, 0))
        self.assert_domain("CAPACITY_CONFLICT", self.store.set_background, "S2", 2, 30)
        self.assertFalse(self.store.set_baseline([{"stationId": "S2", "currentFree": 2}])["initialized"])
        self.advance(11)
        station = self.store.stations()[1]
        self.assertEqual((station["backgroundBusy"], station["charging"], station["currentFree"]), (0, 1, 1))
        self.store.initialize(CATALOG, CITIES)
        self.assertEqual(self.status(owner)["tripId"], trip["tripId"])
        self.assertEqual(self.status(owner)["status"], "CHARGING")

    def test_running_clock_read_synchronizes_and_rejected_command_keeps_progress(self):
        self.store.configure({"reservationTimeoutSeconds": 1})
        owner = self.user()
        trip = self.select(owner)
        self.store.act(owner, trip["tripId"], "arrive")
        self.store.control_clock({"action": "configure", "speed": 60, "paused": False})
        time.sleep(.04)
        self.assert_domain("INVALID_TRANSITION", self.store.act, owner, trip["tripId"], "start")
        self.assertEqual(self.status(owner)["status"], "EXPIRED")
        self.assertEqual(self.store.stations()[0]["currentFree"], 1)

    def test_invalid_configuration_is_atomic_and_clock_monotonic(self):
        before = self.store.admin()["config"]
        self.assert_domain("INVALID_CONFIG", self.store.configure, {"minimumEnergyKwh": 3, "callTimeoutSeconds": -1})
        self.assertEqual(self.store.admin()["config"], before)
        self.assert_domain("INVALID_CONFIG", self.store.configure, {"weights": {"availability": 1}})
        self.assert_domain("INVALID_CONFIG", self.store.configure, {"dailyRewardBudget": float("nan")})
        before = self.store.clock()["time"]
        self.assert_domain("INVALID_CONFIG", self.store.control_clock, {"action": "advance", "seconds": -10})
        self.assertEqual(self.store.clock()["time"], before)
        self.advance(300)
        self.assertGreater(self.store.clock()["time"], before)

    def test_tariff_and_regional_reward_frozen_with_offer(self):
        user = self.user()
        offer = self.offer(user, multiplier=2, price=3.25)
        self.assertEqual(offer["candidates"][0]["rewardPoints"], 200)
        self.store.configure({"rewards": {"first": 1, "second": 1}})
        trip = self.store.select(user, offer["recommendationId"], "S1")
        self.assertEqual(trip["pricePerKwh"], 3.25)
        self.assertEqual(trip["powerKw"], 60)
        self.store.act(user, trip["tripId"], "arrive")
        self.store.act(user, trip["tripId"], "start")
        self.advance(120)
        self.store.act(user, trip["tripId"], "stop")
        paid = self.store.act(user, trip["tripId"], "pay")
        self.assertEqual((paid["amount"], paid["awardedPoints"]), (6.5, 200))
        self.assert_domain("INVALID_RECOMMENDATION", lambda: self.offer(self.user("invalid"), multiplier=1.7))

    def test_feedback_keeps_unserved_wait_null_and_observed_arrival(self):
        queued_user, cancelled_user = self.user(), self.user("取消")
        self.store.set_background("S1", 1, 30)
        queued = self.select(queued_user)
        cancelled = self.select(cancelled_user)
        self.store.act(queued_user, queued["tripId"], "arrive")
        self.store.act(cancelled_user, cancelled["tripId"], "cancel")
        rows = {row["tripId"]: row for row in self.store.feedback()}
        self.assertEqual(rows[queued["tripId"]]["actualFreeAtArrival"], 0)
        self.assertIsNone(rows[queued["tripId"]]["actualWaitMinutes"])
        self.assertIsNone(rows[cancelled["tripId"]]["actualWaitMinutes"])
        self.assertIsNone(rows[cancelled["tripId"]]["arrivedAt"])
        self.advance(30)
        self.store.act(queued_user, queued["tripId"], "confirm")
        self.advance(30)
        self.store.act(queued_user, queued["tripId"], "start")
        rows = self.store.feedback()
        self.assertAlmostEqual(rows[0]["actualWaitMinutes"], 1, delta=.001)
        self.assertEqual(rows[0]["rank"], 1)
        self.assertEqual(rows[0]["source"], "SIMULATED_OPERATIONAL_FEEDBACK")
        self.assertNotIn("token", json.dumps(rows))
        self.assertNotIn("name", json.dumps(rows))

    def test_session_expiry_uses_wall_time_not_replay_time(self):
        session = self.store.create_session("持久演示")
        self.advance(172800)
        self.assertEqual(self.store.authenticate(session["token"]), session["user"]["userId"])
        connection = connect(self.settings)
        try:
            with connection.cursor() as cursor:
                cursor.execute("UPDATE cp_users SET expires_wall=UNIX_TIMESTAMP(UTC_TIMESTAMP(6))-1 WHERE user_id=%s", (session["user"]["userId"],))
            connection.commit()
        finally:
            connection.close()
        self.assert_domain("UNAUTHORIZED", self.store.authenticate, session["token"], status=401)

    def test_energy_threshold_and_cancel_while_charging(self):
        self.store.configure({"minimumEnergyKwh": 2})
        owner = self.user()
        trip = self.start(owner)
        self.assert_domain("INVALID_TRANSITION", self.store.act, owner, trip["tripId"], "cancel")
        self.advance(90)
        self.store.act(owner, trip["tripId"], "stop")
        paid = self.store.act(owner, trip["tripId"], "pay")
        self.assertEqual(paid["rewardStatus"], "BELOW_MINIMUM_ENERGY")
        self.assertEqual(paid["awardedPoints"], 0)

    def test_observed_clock_is_nonmutating_copy_and_thread_local(self):
        before = self.store.clock()
        copy = self.store.observed_clock()
        copy["time"] = "caller mutation"
        self.assertEqual(self.store.observed_clock(), before)

        def worker():
            initial = self.store.observed_clock()
            advanced = self.store.control_clock({"action": "advance", "seconds": 15})
            return initial, advanced, self.store.observed_clock()

        with ThreadPoolExecutor(max_workers=1) as executor:
            initial, advanced, observed = executor.submit(worker).result()
        self.assertIsNone(initial)
        self.assertEqual(observed, advanced)
        self.assertNotEqual(advanced["time"], before["time"])
        self.assertEqual(self.store.observed_clock(), before, "Other worker must not replace caller snapshot")
        self.assertEqual(self.store.clock(), advanced)


if __name__ == "__main__":
    unittest.main()
