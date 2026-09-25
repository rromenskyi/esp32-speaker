"""Write training_parameters.yaml and run microWakeWord training + export.

    python train.py <positives_features_dir> <hard_negatives_features_dir> <steps> [train_dir]
"""
import subprocess, sys, yaml

pos, hard, steps = sys.argv[1], sys.argv[2], int(sys.argv[3])
train_dir = sys.argv[4] if len(sys.argv) > 4 else "trained_models/wakeword"
feat = lambda d, w, p, truth, trunc: {"features_dir": d, "sampling_weight": w, "penalty_weight": p,
                                      "truth": truth, "truncation_strategy": trunc, "type": "mmap"}
config = {
    "window_step_ms": 10,
    "train_dir": train_dir,
    "features": [
        feat(pos, 2.0, 1.0, True, "truncate_start"),
        feat(hard, 4.0, 2.0, False, "random"),                  # synthetic confusable words
        feat("feat_fleurs_ru", 6.0, 1.0, False, "random"),      # Russian read speech
        feat("negative_datasets/speech", 8.0, 1.0, False, "random"),
        feat("negative_datasets/dinner_party", 8.0, 1.0, False, "random"),
        feat("negative_datasets/no_speech", 5.0, 1.0, False, "random"),
        feat("negative_datasets/dinner_party_eval", 0.0, 1.0, False, "split"),   # validation/test only
    ],
    "training_steps": [steps],
    "positive_class_weight": [1],
    "negative_class_weight": [20],
    "learning_rates": [0.001],
    "batch_size": 128,
    "time_mask_max_size": [0], "time_mask_count": [0], "freq_mask_max_size": [0], "freq_mask_count": [0],
    "eval_step_interval": 500,
    "clip_duration_ms": 1500,
    "target_minimization": 0.9,
    "minimization_metric": None,
    "maximization_metric": "average_viable_recall",
}
yaml.safe_dump(config, open("training_parameters.yaml", "w"))
subprocess.run([sys.executable, "-m", "microwakeword.model_train_eval",
                "--training_config=training_parameters.yaml", "--train", "1", "--restore_checkpoint", "1",
                "--test_tf_nonstreaming", "0", "--test_tflite_nonstreaming", "0",
                "--test_tflite_nonstreaming_quantized", "0", "--test_tflite_streaming", "0",
                "--test_tflite_streaming_quantized", "1", "--use_weights", "best_weights",
                "mixednet", "--pointwise_filters", "64,64,64,64", "--repeat_in_block", "1, 1, 1, 1",
                "--mixconv_kernel_sizes", "[5], [7,11], [9,15], [23]", "--residual_connection", "0,0,0,0",
                "--first_conv_filters", "32", "--first_conv_kernel_size", "5", "--stride", "3"], check=True)
