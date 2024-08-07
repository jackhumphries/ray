import tensorflow as tf
from typing import Dict, TYPE_CHECKING

from ray.rllib.core.columns import Columns
from ray.rllib.core.learner.tf.tf_learner import TfLearner
from ray.rllib.core.testing.testing_learner import BaseTestingLearner
from ray.rllib.utils.nested_dict import NestedDict
from ray.rllib.utils.typing import ModuleID, TensorType

if TYPE_CHECKING:
    from ray.rllib.algorithms.algorithm_config import AlgorithmConfig


class BCTfLearner(TfLearner, BaseTestingLearner):
    def compute_loss_for_module(
        self,
        *,
        module_id: ModuleID,
        config: "AlgorithmConfig",
        batch: NestedDict,
        fwd_out: Dict[str, TensorType],
    ) -> TensorType:
        BaseTestingLearner.compute_loss_for_module(
            self,
            module_id=module_id,
            config=config,
            batch=batch,
            fwd_out=fwd_out,
        )
        action_dist_inputs = fwd_out[Columns.ACTION_DIST_INPUTS]
        action_dist_class = self._module[module_id].get_train_action_dist_cls()
        action_dist = action_dist_class.from_logits(action_dist_inputs)
        loss = -tf.math.reduce_mean(action_dist.logp(batch[Columns.ACTIONS]))

        return loss
