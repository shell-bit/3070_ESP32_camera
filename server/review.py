"""List quarantined events/legacy photos or retry complete inputs after fixing AI."""
import argparse
from store import db
from event_queue import expire_events
from manual_review import decide, ReviewConflict


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--retry', action='store_true', help='Retry complete review events and legacy photos after fixing model problems')
    args = parser.parse_args()
    expire_events()
    with db() as connection:
        events = [dict(row) for row in connection.execute(
            "SELECT id,received,error,expected_frames FROM events WHERE state='review' ORDER BY seq")]
        legacy = [dict(row) for row in connection.execute(
            "SELECT id,received,error FROM jobs WHERE state='review' AND event_key IS NULL ORDER BY seq")]
    for row in events + legacy:
        print(row)
    if args.retry:
        count = 0
        for row in events + legacy:
            try:
                decide(row['id'], 'retry', 'Operator requested retry after fixing model problems.')
                count += 1
            except (ReviewConflict, FileNotFoundError) as error:
                print('Skipped', row['id'], str(error))
        print('Requeued', count, 'events/legacy photos')


if __name__ == '__main__':
    main()
