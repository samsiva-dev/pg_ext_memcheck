// @ts-check
import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';

// https://astro.build/config
export default defineConfig({
	integrations: [
		starlight({
			title: 'pg_ext_memcheck',
			description: 'Catch PostgreSQL memory bugs that Valgrind cannot.',
			social: [
				{ icon: 'github', label: 'GitHub', href: 'https://github.com/samsiva-dev/pg_ext_memcheck' },
			],
			customCss: ['./src/styles/custom.css'],
			sidebar: [
				{
					label: 'Getting Started',
					items: [
						{ label: 'Introduction', slug: 'index' },
						{ label: 'Quickstart', slug: 'quickstart' },
					],
				},
				{
					label: 'Development',
					items: [
						{ label: 'Roadmap & Progress', slug: 'roadmap' },
					],
				},
				{
					label: 'Reference',
					items: [
						{ label: 'Features', slug: 'features' },
						{ label: 'SQL API', slug: 'api' },
						{ label: 'Stress Scenarios', slug: 'scenarios' },
						{ label: 'Architecture', slug: 'architecture' },
						{ label: 'Testing a Leaky Extension', slug: 'testing-buggy-extensions' },
					],
				},
			],
		}),
	],
});
