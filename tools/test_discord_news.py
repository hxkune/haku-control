import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from PIL import Image

import discord_news as news


class NewsTests(unittest.TestCase):
    def test_notes_and_generated_changelog_fallback(self):
        self.assertEqual(news.highlights('## Changes\n* **Fix** [pairing](https://example.com) by @dev in https://example.com/pr\n- Add profiles'), ['Fix pairing', 'Add profiles'])
        self.assertEqual(news.highlights('**Full Changelog**: https://example.com/compare'), [news.FALLBACK])
        self.assertEqual(len(news.highlights('\n'.join('- item' for _ in range(10)))), 3)

    def test_long_text_stays_inside_card(self):
        for line in news.wrapped('verylongword' * 100, news.font(26), 989):
            self.assertLessEqual(news.font(26).getlength(line), 989)

    def test_render_and_multipart_upload(self):
        release = {'tag_name': 'v0.4.37', 'published_at': '2026-10-18T19:45:40Z',
                   'body': '- ' + 'A long release note ' * 100}
        with tempfile.TemporaryDirectory() as folder:
            card = Path(folder) / 'card.png'
            items = news.render(release, card)
            with Image.open(card) as image:
                self.assertEqual(image.size, (1200, 800))
                self.assertEqual(image.format, 'PNG')
            with patch.dict('os.environ', {'DISCORD_NEWS_WEBHOOK_URL': 'https://discord.com/api/webhooks/123/fake'}):
                with patch('urllib.request.urlopen') as send:
                    send.return_value.__enter__.return_value.read.return_value = json.dumps({'id': 'test'}).encode()
                    news.publish(release, card, items)
                    request = send.call_args.args[0]
                    self.assertIn(b'attachment://haku-control-news.png', request.data)
                    self.assertIn(b'"allowed_mentions": {"parse": []}', request.data)
                    self.assertIn(card.read_bytes(), request.data)
                    self.assertTrue(request.full_url.endswith('?wait=true'))

    def test_reject_non_discord_webhook(self):
        with patch.dict('os.environ', {'DISCORD_NEWS_WEBHOOK_URL': 'https://example.com/secret'}):
            with self.assertRaises(ValueError):
                news.publish({}, 'missing.png', [])


if __name__ == '__main__':
    unittest.main()
