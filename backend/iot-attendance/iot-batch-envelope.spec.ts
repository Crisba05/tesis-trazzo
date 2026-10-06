// Regression guard for the offline-queue batch envelope.
//
// The IoT queue is all-or-nothing on the wire: if the batch endpoint answers a
// permanent error, the device retries the same bytes forever and its queue
// never drains. Two separate defects produced exactly that in the field
// (Scenario C, 2026-09-26 / 2026-09-27), so both are pinned here.

import { ValidationPipe, ArgumentMetadata } from '@nestjs/common';
import { plainToInstance } from 'class-transformer';
import { validateSync } from 'class-validator';
import { IotBatchScanEnvelopeDto, IotScanDto } from './dto/iot-scan.dto';

// Mirrors the global pipe configured in src/main.ts.
const pipe = new ValidationPipe({
    whitelist: true,
    forbidNonWhitelisted: true,
    transform: true,
    transformOptions: { enableImplicitConversion: true },
});

const meta: ArgumentMetadata = { type: 'body', metatype: IotBatchScanEnvelopeDto, data: '' };

const GOOD = {
    scanId: '01a0e051-be39-77ba-bde2-cb825aedd857',
    documentNumber: '9990000063',
    scannedAt: '2026-09-26T19:44:07-05:00',
    mode: 'entry',
    decision: 1,
    offline: true,
};
// Written by the firmware when a scan happened after a power cut and before
// NTP had resynced: the timestamp is empty.
const POISONED = { ...GOOD, scanId: 'x', documentNumber: '9990000058', scannedAt: '' };

/** Same check as IotAttendanceService.validateScanRecord. */
function isObjectRecord(raw: unknown): boolean {
    return raw !== null && typeof raw === 'object' && !Array.isArray(raw);
}

describe('IotBatchScanEnvelopeDto', () => {
    it('accepts a batch that contains a malformed record instead of rejecting all of it', async () => {
        await expect(pipe.transform({ scans: [GOOD, POISONED] }, meta)).resolves.toBeDefined();
    });

    it('hands the service plain objects, not coerced arrays', async () => {
        // Without `@Type(() => Object)` on `scans`, implicit conversion turns
        // every record into an Array carrying the record's properties. The
        // service then rejects all of them as "not an object" — which silently
        // dropped six real queued events in the field.
        const dto = (await pipe.transform({ scans: [GOOD, POISONED] }, meta)) as IotBatchScanEnvelopeDto;
        for (const raw of dto.scans) {
            expect(Array.isArray(raw)).toBe(false);
            expect(isObjectRecord(raw)).toBe(true);
        }
        expect((dto.scans[0] as Record<string, unknown>).documentNumber).toBe('9990000063');
    });

    it('isolates the bad record: the valid one still validates', async () => {
        const dto = (await pipe.transform({ scans: [GOOD, POISONED] }, meta)) as IotBatchScanEnvelopeDto;
        const verdicts = dto.scans.map((raw) =>
            validateSync(plainToInstance(IotScanDto, raw, { enableImplicitConversion: true }), {
                whitelist: true,
            }).length,
        );
        expect(verdicts[0]).toBe(0); // good record passes
        expect(verdicts[1]).toBeGreaterThan(0); // empty scannedAt is caught
    });

    it('keeps a record carrying an unknown field from a newer firmware', async () => {
        const future = { ...GOOD, someFieldAddedLater: 42 };
        const dto = (await pipe.transform({ scans: [future] }, meta)) as IotBatchScanEnvelopeDto;
        const errors = validateSync(
            plainToInstance(IotScanDto, dto.scans[0], { enableImplicitConversion: true }),
            { whitelist: true },
        );
        expect(errors).toHaveLength(0);
    });

    it('still rejects an envelope that is structurally wrong', async () => {
        await expect(pipe.transform({ scans: 'not-an-array' }, meta)).rejects.toBeDefined();
        await expect(pipe.transform({ scans: [] }, meta)).rejects.toBeDefined();
    });
});
