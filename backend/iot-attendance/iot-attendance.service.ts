// src/modules/iot-attendance/iot-attendance.service.ts
//
// IoT attendance ingestion. Resolves the active academic schedule from the device's
// assignedScheduleIds, then determines PRESENT/LATE from AttendancePolicy and
// upserts the Attendance record. Idempotent by scanId.
//
// NOTE: this module intentionally duplicates a thin slice of attendance-scan logic
// for v1 to keep the IoT path self-contained. v1.1 will refactor a shared core.

import {
    Injectable,
    BadRequestException,
    Logger,
} from '@nestjs/common';
import { Prisma, AttendanceStatus, AttendanceSource, IotDevice } from '@prisma/client';
import { PrismaService } from '../../prisma/prisma.service';
import { RedisCacheService } from '../../common/services/redis-cache.service';
import { IotEventsService } from '../iot-events/iot-events.service';
import { plainToInstance } from 'class-transformer';
import { validateSync } from 'class-validator';
import { IotScanDto, IotBatchScanEnvelopeDto, IotScanMode } from './dto/iot-scan.dto';

export interface ScanOutcome {
    scanId?: string;
    documentNumber: string;
    status: 'success' | 'late' | 'reinforcement' | 'already_registered' | 'out_of_window' | 'student_not_found' | 'no_active_schedule' | 'invalid' | 'error';
    message: string;
    /** Which window the scan fell into, echoed back so firmware can log/display. */
    resolvedMode?: 'entry' | 'exit' | 'reinforcement' | null;
    studentName?: string | null;
    classroomLabel?: string | null;
    levelName?: string | null;
    shiftName?: string | null;
    attendanceStatus?: AttendanceStatus | null;
    scheduleId?: string | null;
}

const IDEMPOTENCY_TTL_SECONDS = 24 * 60 * 60; // 24h

// Anything older than this cannot be a real scan from a deployed device; it is
// the epoch placeholder a module writes when it has no clock at all.
const IMPLAUSIBLE_BEFORE_MS = Date.UTC(2025, 0, 1);

@Injectable()
export class IotAttendanceService {
    private readonly logger = new Logger(IotAttendanceService.name);

    constructor(
        private readonly prisma: PrismaService,
        private readonly redis: RedisCacheService,
        private readonly events: IotEventsService,
    ) { }

    async processScan(device: IotDevice, dto: IotScanDto): Promise<ScanOutcome> {
        const scannedAt = this.resolveScannedAt(device, dto);

        // 1. Idempotency check
        if (dto.scanId) {
            const fresh = await this.redis.setIfAbsent(
                device.tenantId,
                `iot:scan:idem:${dto.scanId}`,
                1,
                IDEMPOTENCY_TTL_SECONDS,
            );
            if (this.redis.isEnabled() && !fresh) {
                return {
                    scanId: dto.scanId,
                    documentNumber: dto.documentNumber,
                    status: 'already_registered',
                    message: 'Scan already processed (idempotent)',
                };
            }
        }

        // 2. Look up student first (a module now covers ALL students of the
        //    tenant; the schedule is derived from the student's own classroom,
        //    not from device.assignedScheduleIds anymore).
        const student = await this.findStudentByDocument(device.tenantId, dto.documentNumber);
        if (!student) {
            return this.outcome(dto, 'student_not_found', 'Student not found');
        }

        if (!student.schedule || !student.schedule.attendancePolicy) {
            return this.outcome(
                dto,
                'no_active_schedule',
                'Student has no attendance policy configured on their schedule',
                student,
            );
        }

        // 3. Auto-detect the mode from the current time using the student's
        //    schedule policy. The `dto.mode` hint from firmware is used only
        //    as a fallback when the time falls between windows.
        const detectedMode = this.autoDetectMode(
            student.schedule.attendancePolicy,
            scannedAt,
            dto.mode ?? IotScanMode.ENTRY,
        );

        // 4. Classify with the student's policy in the detected mode.
        const decision = this.classifyScan(student.schedule.attendancePolicy, scannedAt, detectedMode);
        if (decision.status === 'out_of_window') {
            return this.outcome(dto, 'out_of_window', decision.message, student, student.schedule, detectedMode);
        }

        // 5. Upsert Attendance
        const dateOnly = peruDateOnly(scannedAt);
        const auditSource = dto.offline ? AttendanceSource.IOT_OFFLINE : AttendanceSource.IOT_ONLINE;
        try {
            await this.upsertAttendance({
                tenantId: device.tenantId,
                studentId: student.id,
                date: dateOnly,
                mode: detectedMode,
                scannedAt,
                status: decision.attendanceStatus,
                source: auditSource,
                sourceDeviceId: device.id,
            });
        } catch (err) {
            this.logger.warn(`Attendance upsert failed: ${(err as Error).message}`);
            return this.outcome(dto, 'error', 'Failed to register attendance', student, student.schedule, detectedMode);
        }

        // Emit live event so panel sees the scan in real time
        const studentName = `${student.firstName ?? ''} ${student.lastName ?? ''}`.trim();
        await this.events.emitToTenant(device.tenantId, 'iot.device.scan', {
            deviceId: device.deviceId,
            studentName,
            classroomLabel: student.classroomLabel,
            attendanceStatus: decision.attendanceStatus,
            status: decision.status,
            mode: detectedMode,
            scannedAt: scannedAt.toISOString(),
        });

        return {
            scanId: dto.scanId,
            documentNumber: dto.documentNumber,
            status: decision.status,
            message: decision.message,
            resolvedMode: detectedMode,
            studentName,
            classroomLabel: student.classroomLabel,
            levelName: student.schedule.levelName,
            shiftName: student.schedule.shiftName,
            attendanceStatus: decision.attendanceStatus,
            scheduleId: student.schedule.id,
        };
    }

    /**
     * The device has no battery-backed clock. After a power cut it restores the
     * time from NVS, and on a brand-new unit that has never synced there is no
     * time at all — those scans arrive stamped with the epoch placeholder and
     * `ntpUncertain`. Persisting them imprecisely beats dropping them, so we
     * substitute our own receive time here rather than reject the record.
     *
     * Only applied to timestamps that are implausible for a school year; a
     * merely approximate one (restored clock, off by minutes) is kept as sent,
     * since it is closer to the truth than the moment the network came back.
     */
    private resolveScannedAt(device: IotDevice, dto: IotScanDto): Date {
        const parsed = new Date(dto.scannedAt);
        if (!Number.isNaN(parsed.getTime()) && parsed.getTime() >= IMPLAUSIBLE_BEFORE_MS) {
            return parsed;
        }
        this.logger.warn(
            `Device ${device.deviceId}: scan ${dto.scanId ?? '?'} (${dto.documentNumber}) ` +
            `arrived without a usable clock (scannedAt=${dto.scannedAt}); using receive time`,
        );
        return new Date();
    }

    /**
     * Validate a single queued record on its own, so one bad record cannot
     * invalidate the batch. Returns the typed DTO, or the reason it was
     * rejected. See IotBatchScanEnvelopeDto for why the global ValidationPipe
     * must not do this.
     */
    private validateScanRecord(raw: unknown): { dto: IotScanDto } | { reason: string } {
        if (raw === null || typeof raw !== 'object' || Array.isArray(raw)) {
            return { reason: 'Record is not an object' };
        }
        const dto = plainToInstance(IotScanDto, raw, { enableImplicitConversion: true });
        // No `forbidNonWhitelisted`: an unknown extra field from a newer
        // firmware must not cost us the record. Unknown fields are ignored.
        const errors = validateSync(dto, { whitelist: true });
        if (errors.length > 0) {
            const reason = errors
                .map((e) => Object.values(e.constraints ?? {}).join('; '))
                .filter(Boolean)
                .join(' | ');
            return { reason: reason || 'Record failed validation' };
        }
        return { dto };
    }

    async processBatch(device: IotDevice, dto: IotBatchScanEnvelopeDto): Promise<{
        processed: number;
        accepted: number;
        rejected: number;
        results: ScanOutcome[];
    }> {
        const results: ScanOutcome[] = [];
        for (const raw of dto.scans) {
            const checked = this.validateScanRecord(raw);
            const record = raw as { documentNumber?: unknown; scanId?: unknown } | null;
            const documentNumber = typeof record?.documentNumber === 'string' ? record.documentNumber : '';
            const scanId = typeof record?.scanId === 'string' ? record.scanId : undefined;

            if ('reason' in checked) {
                this.logger.warn(
                    `Device ${device.deviceId}: rejecting malformed queued record ` +
                    `(${documentNumber || 'no documentNumber'}): ${checked.reason}`,
                );
                results.push({ scanId, documentNumber, status: 'invalid', message: checked.reason });
                continue;
            }

            try {
                results.push(await this.processScan(device, checked.dto));
            } catch (err) {
                results.push({
                    scanId,
                    documentNumber,
                    status: 'error',
                    message: (err as Error).message ?? 'Unknown error',
                });
            }
        }

        const rejected = results.filter((r) => r.status === 'invalid').length;
        const accepted = results.length - rejected;

        // Update offline counter desnormalizado
        await this.prisma.iotDevice.update({
            where: { id: device.id },
            data: {
                totalScansToday: { increment: accepted },
                pendingOfflineRecords: 0, // backend adjudicated the whole buffer
            },
        }).catch(() => { /* swallow */ });

        return { processed: results.length, accepted, rejected, results };
    }

    // ----- helpers -----

    private outcome(
        dto: IotScanDto,
        status: ScanOutcome['status'],
        message: string,
        student?: { id: string; firstName: string | null; lastName: string | null; classroomLabel?: string | null } | null,
        schedule?: { id: string; levelName: string | null; shiftName: string | null } | null,
        resolvedMode?: 'entry' | 'exit' | 'reinforcement' | null,
    ): ScanOutcome {
        return {
            scanId: dto.scanId,
            documentNumber: dto.documentNumber,
            status,
            message,
            resolvedMode: resolvedMode ?? null,
            studentName: student ? `${student.firstName ?? ''} ${student.lastName ?? ''}`.trim() : null,
            classroomLabel: student?.classroomLabel ?? null,
            levelName: schedule?.levelName ?? null,
            shiftName: schedule?.shiftName ?? null,
            attendanceStatus: null,
            scheduleId: schedule?.id ?? null,
        };
    }

    /**
     * Look up student by DNI within the tenant, hydrating their schedule +
     * attendance policy. The scan mode + classification will be based on
     * THIS student's schedule (not the device's).
     */
    private async findStudentByDocument(tenantId: string, documentNumber: string) {
        const normalized = documentNumber.trim().toUpperCase();
        const student = await this.prisma.student.findFirst({
            where: {
                tenantId,
                documentNumber: normalized,
                isActive: true,
            },
            include: {
                classroom: {
                    select: {
                        section: true,
                        grade: { select: { name: true } },
                        academicSchedule: {
                            include: {
                                attendancePolicy: true,
                                level: { select: { name: true } },
                                shift: { select: { name: true } },
                            },
                        },
                    },
                },
            },
        });

        if (!student) return null;

        const classroomLabel = student.classroom
            ? `${student.classroom.grade?.name ?? ''} ${student.classroom.section ?? ''}`.trim()
            : null;

        const sch = student.classroom?.academicSchedule ?? null;
        const schedule = sch
            ? {
                  id: sch.id,
                  name: sch.name,
                  levelName: sch.level?.name ?? null,
                  shiftName: sch.shift?.name ?? null,
                  attendancePolicy: sch.attendancePolicy,
              }
            : null;

        return {
            id: student.id,
            firstName: student.firstName,
            lastName: student.lastName,
            classroomLabel,
            schedule,
        };
    }

    /**
     * Choose which mode window applies at the current time. Priority order:
     * ENTRY (with -30/+15 tolerance) → REINFORCEMENT → EXIT → fallback to
     * whatever the firmware suggested (used when time is out of any window,
     * so classifyScan can still report 'out_of_window' meaningfully).
     */
    private autoDetectMode(
        policy: { entryTime: string | null; lateTime: string | null; entryLimitTime: string | null;
                  enableExit: boolean; exitTime: string | null; exitLimitTime: string | null;
                  enableReinforcement: boolean; reinforcementStart: string | null; reinforcementEnd: string | null },
        scannedAt: Date,
        fallback: IotScanMode,
    ): IotScanMode {
        const scanMinutes = getPeruMinutes(scannedAt);
        const TOL_BEFORE = 30;
        const TOL_AFTER = 15;

        // ENTRY window
        const entryStart = parseHHMMtoMinutes(policy.entryTime);
        const entryEnd = parseHHMMtoMinutes(policy.entryLimitTime);
        if (entryStart !== null && entryEnd !== null &&
            scanMinutes >= entryStart - TOL_BEFORE && scanMinutes <= entryEnd + TOL_AFTER) {
            return IotScanMode.ENTRY;
        }

        // EXIT window
        if (policy.enableExit) {
            const exitStart = parseHHMMtoMinutes(policy.exitTime);
            const exitEnd = parseHHMMtoMinutes(policy.exitLimitTime);
            if (exitStart !== null && exitEnd !== null &&
                scanMinutes >= exitStart - TOL_BEFORE && scanMinutes <= exitEnd + TOL_AFTER) {
                return IotScanMode.EXIT;
            }
        }

        return fallback === IotScanMode.REINFORCEMENT ? IotScanMode.ENTRY : fallback;
    }

    private classifyScan(
        policy: { entryTime: string | null; lateTime: string | null; entryLimitTime: string | null;
                  enableExit: boolean; exitTime: string | null; exitLimitTime: string | null;
                  enableReinforcement: boolean; reinforcementStart: string | null; reinforcementEnd: string | null },
        scannedAt: Date,
        mode: IotScanMode,
    ): { status: ScanOutcome['status']; message: string; attendanceStatus: AttendanceStatus } {
        const scanMinutes = getPeruMinutes(scannedAt);

        if (mode === IotScanMode.EXIT) {
            const exitStart = parseHHMMtoMinutes(policy.exitTime);
            const exitEnd = parseHHMMtoMinutes(policy.exitLimitTime);
            if (!policy.enableExit) {
                return { status: 'error', message: 'Exit registration disabled by policy', attendanceStatus: AttendanceStatus.PRESENT };
            }
            if (exitStart !== null && scanMinutes < exitStart) {
                return { status: 'out_of_window', message: 'Too early for exit', attendanceStatus: AttendanceStatus.PRESENT };
            }
            if (exitEnd !== null && scanMinutes > exitEnd) {
                return { status: 'success', message: 'Late exit registered', attendanceStatus: AttendanceStatus.EARLY_DEPARTURE };
            }
            return { status: 'success', message: 'Exit registered', attendanceStatus: AttendanceStatus.PRESENT };
        }

        if (mode === IotScanMode.REINFORCEMENT) {
            if (!policy.enableReinforcement) {
                return { status: 'error', message: 'Reinforcement disabled by policy', attendanceStatus: AttendanceStatus.PRESENT };
            }
            const refStart = parseHHMMtoMinutes(policy.reinforcementStart);
            const refEnd = parseHHMMtoMinutes(policy.reinforcementEnd);
            if (refStart !== null && scanMinutes < refStart - 30) {
                return { status: 'out_of_window', message: 'Too early for reinforcement', attendanceStatus: AttendanceStatus.PRESENT };
            }
            if (refEnd !== null && scanMinutes > refEnd + 15) {
                return { status: 'out_of_window', message: 'Past reinforcement window', attendanceStatus: AttendanceStatus.PRESENT };
            }
            return { status: 'reinforcement', message: 'REFORZAMIENTO registered', attendanceStatus: AttendanceStatus.PRESENT };
        }

        // ENTRY
        const lateStart = parseHHMMtoMinutes(policy.lateTime);
        const limitEnd = parseHHMMtoMinutes(policy.entryLimitTime);

        if (limitEnd !== null && scanMinutes > limitEnd) {
            return { status: 'out_of_window', message: 'Past entry limit — mark as ABSENT manually', attendanceStatus: AttendanceStatus.ABSENT };
        }

        if (lateStart !== null && scanMinutes >= lateStart) {
            return { status: 'late', message: 'TARDANZA registered', attendanceStatus: AttendanceStatus.LATE };
        }

        return { status: 'success', message: 'PRESENTE registered', attendanceStatus: AttendanceStatus.PRESENT };
    }

    private async upsertAttendance(params: {
        tenantId: string;
        studentId: string;
        date: Date;
        mode: IotScanMode;
        scannedAt: Date;
        status: AttendanceStatus;
        source: AttendanceSource;
        sourceDeviceId: string;
    }) {
        const { tenantId, studentId, date, mode, scannedAt, status, source, sourceDeviceId } = params;

        // The rest of the schema (attendance-manual, frontend renderers)
        // uses the "Peru wall clock stored as UTC" convention: 13:03 Lima is
        // written as 13:03 UTC and read back with getUTCHours(). We must
        // encode scannedAt the same way, otherwise the frontend shows
        // scannedAt + 5h (Lima UTC-5 shifted twice).
        const wallClock = toPeruWallClockUTC(scannedAt);

        if (mode === IotScanMode.EXIT) {
            // Only update timeOut on an existing record; don't create a new attendance for exit-only.
            const existing = await this.prisma.attendance.findUnique({
                where: { tenantId_studentId_date: { tenantId, studentId, date } },
            });
            if (!existing) {
                throw new BadRequestException('Cannot register exit without an entry');
            }
            if (existing.timeOut) {
                throw new BadRequestException('Exit already registered');
            }
            await this.prisma.attendance.update({
                where: { id: existing.id },
                data: { timeOut: wallClock },
                // No pisamos source: la fila conserva el origen de la entrada real.
            });
            return;
        }

        // ENTRY or REINFORCEMENT: upsert with status.
        // Reinforcement uses the same shape (timeIn + PRESENT); the mode is
        // preserved on the emitted event so the panel can badge it distinctly.
        //
        // Si la fila ya existía como SYSTEM_AUTO_ABSENCE (cron generó falta antes de
        // recibir este batch offline del IoT), sobreescribimos source y limpiamos
        // el notes auto-generado.
        const existingRow = await this.prisma.attendance.findUnique({
            where: { tenantId_studentId_date: { tenantId, studentId, date } },
            select: { source: true, notes: true },
        });
        const clearAutoAbsenceNotes =
            existingRow?.source === AttendanceSource.SYSTEM_AUTO_ABSENCE ||
            (existingRow?.notes ?? '').startsWith('Falta generada automaticamente');

        await this.prisma.attendance.upsert({
            where: { tenantId_studentId_date: { tenantId, studentId, date } },
            create: {
                tenantId,
                studentId,
                date,
                timeIn: wallClock,
                status,
                source,
                sourceDeviceId,
                sourceScannedAt: scannedAt,
            },
            update: {
                // Only set timeIn if not already set, never downgrade status
                ...(status === AttendanceStatus.PRESENT
                    ? { timeIn: wallClock, status: AttendanceStatus.PRESENT }
                    : {}),
                source,
                sourceDeviceId,
                sourceScannedAt: scannedAt,
                ...(clearAutoAbsenceNotes ? { notes: null } : {}),
            },
        });
    }
}

// Convert a real UTC Date to a Date whose UTC components equal the Peru
// wall-clock components (Peru = UTC-5, no DST). Matches the convention used
// by attendance-manual's toPeruDate + formatPeruTime helpers.
function toPeruWallClockUTC(date: Date): Date {
    return new Date(date.getTime() - 5 * 60 * 60_000);
}

// ----- pure helpers -----

function parseHHMMtoMinutes(value: string | null | undefined): number | null {
    if (!value) return null;
    const match = /^(\d{2}):(\d{2})$/.exec(value);
    if (!match) return null;
    return Number(match[1]) * 60 + Number(match[2]);
}

function getPeruMinutes(date: Date): number {
    // Peru is UTC-5 year-round
    const peruDate = new Date(date.getTime() - 5 * 60 * 60_000);
    return peruDate.getUTCHours() * 60 + peruDate.getUTCMinutes();
}

function peruDateOnly(date: Date): Date {
    const peruDate = new Date(date.getTime() - 5 * 60 * 60_000);
    return new Date(Date.UTC(peruDate.getUTCFullYear(), peruDate.getUTCMonth(), peruDate.getUTCDate()));
}
