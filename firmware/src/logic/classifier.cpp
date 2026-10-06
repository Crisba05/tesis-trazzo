// src/logic/classifier.cpp
#include "logic/classifier.h"

namespace classifier {

    AttendanceDecision classify(int16_t minutesNow, const ScheduleRecord& s) {
        if (s.entryTime < 0) return AttendanceDecision::JORNADA_NO_ACTIVA;

        // Entry window: [entryTime, lateTime)
        if (s.lateTime > 0 && minutesNow >= s.entryTime && minutesNow < s.lateTime) {
            return AttendanceDecision::PRESENTE;
        }

        // Late window: [lateTime, entryLimitTime]
        int16_t limit = (s.entryLimitTime > 0) ? s.entryLimitTime
                      : (s.lateTime > 0) ? s.lateTime + 60 : s.entryTime + 120;
        if (s.lateTime > 0 && minutesNow >= s.lateTime && minutesNow <= limit) {
            return AttendanceDecision::TARDANZA;
        }

        // If no lateTime defined, everything in [entryTime, limit] is PRESENTE
        if (s.lateTime <= 0 && minutesNow >= s.entryTime && minutesNow <= limit) {
            return AttendanceDecision::PRESENTE;
        }

        // Reforzamiento window (evaluated BEFORE exit — reinforcement is
        // typically after regular class hours but before formal exit windows).
        if (s.enableReinforcement && s.reinforcementStart > 0 && s.reinforcementEnd > 0) {
            if (minutesNow >= s.reinforcementStart && minutesNow <= s.reinforcementEnd) {
                return AttendanceDecision::REFORZAMIENTO;
            }
        }

        // Exit window
        if (s.enableExit && s.exitTime > 0) {
            int16_t exitEnd = (s.exitLimitTime > 0) ? s.exitLimitTime : s.exitTime + 60;
            if (minutesNow >= s.exitTime && minutesNow <= exitEnd) {
                return AttendanceDecision::SALIDA;
            }
        }

        return AttendanceDecision::FUERA_DE_HORA;
    }
}
