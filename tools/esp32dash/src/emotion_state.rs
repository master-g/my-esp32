use crate::emotion::{Emotion, EmotionAnalysis};

// Scoring follows notchi's EmotionState: each message only nudges a score, so it
// takes a few messages in a row to change the mood and sustained ones to escalate.
const HAPPY_THRESHOLD: f32 = 0.6;
const SAD_THRESHOLD: f32 = 0.45;
const ESCALATION_THRESHOLD: f32 = 0.9;
const INTENSITY_DAMPEN: f32 = 0.5;
const DECAY_RATE: f32 = 0.92;
const INTER_EMOTION_DECAY: f32 = 0.9;
const NEUTRAL_COUNTER_DECAY: f32 = 0.85;

#[derive(Debug, Clone, Default)]
pub struct EmotionState {
    happy_score: f32,
    sad_score: f32,
}

impl EmotionState {
    pub fn record_emotion(&mut self, analysis: &EmotionAnalysis) {
        let gain = analysis.intensity * INTENSITY_DAMPEN;
        match analysis.emotion {
            Emotion::Happy | Emotion::Elated => {
                self.happy_score = (self.happy_score + gain).min(1.0);
                self.sad_score *= INTER_EMOTION_DECAY;
            }
            Emotion::Sad | Emotion::Sob => {
                self.sad_score = (self.sad_score + gain).min(1.0);
                self.happy_score *= INTER_EMOTION_DECAY;
            }
            Emotion::Neutral => {
                self.happy_score *= NEUTRAL_COUNTER_DECAY;
                self.sad_score *= NEUTRAL_COUNTER_DECAY;
            }
        }
    }

    /// Called once a minute.
    pub fn decay(&mut self) {
        self.happy_score *= DECAY_RATE;
        self.sad_score *= DECAY_RATE;
        if self.happy_score < 0.01 {
            self.happy_score = 0.0;
        }
        if self.sad_score < 0.01 {
            self.sad_score = 0.0;
        }
    }

    pub fn current_emotion(&self) -> Emotion {
        if self.happy_score > self.sad_score {
            if self.happy_score >= ESCALATION_THRESHOLD {
                Emotion::Elated
            } else if self.happy_score >= HAPPY_THRESHOLD {
                Emotion::Happy
            } else {
                Emotion::Neutral
            }
        } else if self.sad_score >= ESCALATION_THRESHOLD {
            Emotion::Sob
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

    fn record(state: &mut EmotionState, emotion: Emotion, intensity: f32) {
        state.record_emotion(&EmotionAnalysis {
            emotion,
            intensity,
        });
    }

    #[test]
    fn one_happy_message_is_not_enough_but_two_are() {
        let mut state = EmotionState::default();
        record(&mut state, Emotion::Happy, 1.0);
        assert_eq!(state.current_emotion(), Emotion::Neutral);
        record(&mut state, Emotion::Happy, 0.4);
        assert_eq!(state.current_emotion(), Emotion::Happy);
    }

    #[test]
    fn sustained_praise_escalates_to_elated() {
        let mut state = EmotionState::default();
        record(&mut state, Emotion::Happy, 1.0);
        record(&mut state, Emotion::Happy, 0.7);
        assert_eq!(state.current_emotion(), Emotion::Happy);
        record(&mut state, Emotion::Happy, 0.2);
        assert_eq!(state.current_emotion(), Emotion::Elated);
    }

    #[test]
    fn one_strong_sad_message_shows_sad_and_two_show_sob() {
        let mut state = EmotionState::default();
        record(&mut state, Emotion::Sad, 0.9);
        assert_eq!(state.current_emotion(), Emotion::Sad);
        record(&mut state, Emotion::Sad, 0.9);
        assert_eq!(state.current_emotion(), Emotion::Sob);
    }

    #[test]
    fn neutral_messages_and_time_bring_the_mood_back() {
        let mut state = EmotionState::default();
        record(&mut state, Emotion::Sad, 1.0);
        record(&mut state, Emotion::Neutral, 0.0);
        assert_eq!(state.current_emotion(), Emotion::Neutral);

        let mut state = EmotionState::default();
        record(&mut state, Emotion::Happy, 1.0);
        record(&mut state, Emotion::Happy, 1.0);
        assert_eq!(state.current_emotion(), Emotion::Elated);
        state.decay();
        state.decay();
        assert_eq!(state.current_emotion(), Emotion::Happy);
        for _ in 0..60 {
            state.decay();
        }
        assert_eq!(state.current_emotion(), Emotion::Neutral);
    }

    #[test]
    fn the_other_mood_fades_when_one_is_recorded() {
        let mut state = EmotionState::default();
        record(&mut state, Emotion::Sad, 1.0);
        record(&mut state, Emotion::Happy, 1.0);
        record(&mut state, Emotion::Happy, 0.3);
        assert_eq!(state.current_emotion(), Emotion::Happy);
    }
}
