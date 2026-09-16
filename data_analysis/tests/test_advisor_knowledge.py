"""Method retrieval stays relevant, small, and tied to maintained sources."""
from pathlib import Path
import unittest

from data_analysis.ml.advisor import knowledge


METHOD_IDS = {
    "knowledge.period_comparison", "knowledge.supply_demand", "knowledge.action_validation",
    "knowledge.composition", "knowledge.priority_control", "knowledge.strategy_limits",
}


class AdvisorKnowledgeTests(unittest.TestCase):
    def test_six_method_passages_have_unique_ids_short_text_and_real_source_sections(self):
        self.assertEqual(len(knowledge.PASSAGES), 16)
        self.assertEqual(len({row["id"] for row in knowledge.PASSAGES}), 16)
        root = Path(__file__).resolve().parents[2]
        methods = [row for row in knowledge.PASSAGES if row["id"] in METHOD_IDS]
        self.assertEqual(len(methods), 6)
        for row in knowledge.PASSAGES:
            self.assertLessEqual(len(row["text"]), 1200)
            self.assertTrue(row["text"].strip())
        for row in methods:
            source, section = row["source"].split(" · ", 1)
            self.assertEqual(source, "data_analysis/docs/advisor_analysis.md")
            self.assertIn("## " + section, (root / source).read_text(encoding="utf8"))

    def test_short_operational_advice_questions_retrieve_methods(self):
        for question in ("怎么改进", "给建议", "优先处理", "运营策略"):
            with self.subTest(question=question):
                result = knowledge.retrieve(question)
                self.assertTrue(METHOD_IDS.intersection(row["id"] for row in result))
                self.assertLessEqual(len(result), 4)

    def test_specific_method_questions_match_the_corresponding_passage(self):
        cases = {
            "紧邻完整等长前期如何对比？百分点与负数现金怎么解释？": "knowledge.period_comparison",
            "供需与失败原因分解，无可用桩、等待离队和叫号未确认怎么排查？": "knowledge.supply_demand",
            "怎么改进？提出行动后用哪些验证指标判断是否改善？": "knowledge.action_validation",
            "群体构成和辛普森偏差会怎样影响总体方向？": "knowledge.composition",
            "优先处理哪个站？样本门槛和独立对照怎么选择？": "knowledge.priority_control",
            "合理运营建议能否保证收益或者挽回订单？": "knowledge.strategy_limits",
        }
        for question, expected in cases.items():
            with self.subTest(question=question):
                self.assertIn(expected, [row["id"] for row in knowledge.retrieve(question)])

    def test_topic_hints_cannot_force_unrelated_questions_into_operational_knowledge(self):
        for question in ("xyzzzzquux", "quasar nebula orbital", "天文望远镜", "寿司刺身寿喜烧",
                         "能不能介绍寿司刺身？", "能不能给我讲讲星际飞船？"):
            for topics in ([], ["overview", "stations", "bottlenecks", "behavior", "models"]):
                with self.subTest(question=question, topics=topics):
                    self.assertEqual(knowledge.retrieve(question, topics), [])

    def test_retrieval_bounds_and_copy_isolation_remain_intact(self):
        question = "运营策略怎么改进，供需、失败原因、优先处理、群体构成和等长前期比较"
        self.assertEqual(len(knowledge.retrieve(question, limit=99)), 4)
        self.assertEqual(knowledge.retrieve(question, limit=0), [])
        before = knowledge.retrieve(question)
        changed = knowledge.retrieve(question)
        changed[0]["text"] = "changed"
        self.assertEqual(knowledge.retrieve(question), before)
        self.assertTrue(all(set(row) == {"id", "title", "text", "source"} for row in before))

    def test_existing_metric_retrieval_stays_specific_after_adding_methods(self):
        self.assertEqual(knowledge.retrieve("充电利用率怎么算？完整小时缺测和分母", ["overview"])[0]["id"],
                         "knowledge.utilization")
        self.assertEqual(knowledge.retrieve("补能间隔首次观察前序历史和用户类型", ["behavior"])[0]["id"],
                         "knowledge.behavior")


if __name__ == "__main__":
    unittest.main()
