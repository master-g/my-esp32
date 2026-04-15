use crate::emotion::{Emotion, EmotionAnalysis};

const EMOTION_DECAY_FACTOR: f32 = 0.75;
const HAPPY_THRESHOLD: f32 = 0.35;
const SAD_THRESHOLD: f32 = 0.35;
const SOB_THRESHOLD: f32 = 0.8;

#[derive(Debug, Clone, Default)]
pub struct EmotionState {
    happy_score: f32,
    sad_score: f32,
}

impl EmotionState {
    pub fn record_emotion(&mut self, analysis: &EmotionAnalysis) {
        match analysis.emotion {
            Emotion::Happy => {
                self.happy_score = self.happy_score.max(analysis.confidence.max(HAPPY_THRESHOLD));
                self.sad_score *= 0.5;
            }
            Emotion::Sad => {
                self.sad_score = self.sad_score.max(analysis.confidence.max(SAD_THRESHOLD));
                self.happy_score *= 0.6;
            }
            Emotion::Sob => {
                self.sad_score =
                    self.sad_score.max((analysis.confidence.max(SOB_THRESHOLD)).min(1.0));
                self.happy_score *= 0.4;
            }
            Emotion::Neutral => {
                self.happy_score *= 0.8;
                self.sad_score *= 0.8;
            }
        }
    }

    pub fn decay(&mut self) {
        self.happy_score *= EMOTION_DECAY_FACTOR;
        self.sad_score *= EMOTION_DECAY_FACTOR;
        if self.happy_score < 0.05 {
            self.happy_score = 0.0;
        }
        if self.sad_score < 0.05 {
            self.sad_score = 0.0;
        }
    }

    pub fn current_emotion(&self) -> Emotion {
        if self.sad_score >= SOB_THRESHOLD {
            Emotion::Sob
        } else if self.happy_score >= HAPPY_THRESHOLD && self.happy_score > self.sad_score {
            Emotion::Happy
        } else if self.sad_score >= SAD_THRESHOLD {
            Emotion::Sad
        } else {
            Emotion::Neutral
        }
    }
}

#[cfg(test)]
mod tests {
    use crate::emotion::{Emotion, EmotionAnalysis};

    use super::EmotionState;

    #[test]
    fn happy_analysis_turns_state_happy() {
        let mut state = EmotionState::default();
        state.record_emotion(&EmotionAnalysis {
            emotion: Emotion::Happy,
            confidence: 0.7,
        });
        assert_eq!(state.current_emotion(), Emotion::Happy);
    }

    #[test]
    fn sob_analysis_turns_state_sob() {
        let mut state = EmotionState::default();
        state.record_emotion(&EmotionAnalysis {
            emotion: Emotion::Sob,
            confidence: 0.9,
        });
        assert_eq!(state.current_emotion(), Emotion::Sob);
    }

    #[test]
    fn decay_brings_scores_back_to_neutral() {
        let mut state = EmotionState::default();
        state.record_emotion(&EmotionAnalysis {
            emotion: Emotion::Happy,
            confidence: 0.7,
        });
        for _ in 0..10 {
            state.decay();
        }
        assert_eq!(state.current_emotion(), Emotion::Neutral);
    }
}
